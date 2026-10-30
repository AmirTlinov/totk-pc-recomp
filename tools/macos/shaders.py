"""Build the native shader service and SPIRV-Cross from pinned sources."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

import build_xlate
from macos.build import BUILD, ROOT, digest, run, stage_sources, write, library_prefix
from macos.prepare import link

SPIRV_PIN = 'aa217aeb6c9f0ace7a0ab233b28807edf45eb165'
SPIRV_SOURCES = ('main.cpp spirv_cfg.cpp spirv_cpp.cpp spirv_cross.cpp spirv_cross_parsed_ir.cpp '
                 'spirv_cross_util.cpp spirv_glsl.cpp spirv_hlsl.cpp spirv_msl.cpp spirv_parser.cpp spirv_reflect.cpp').split()


def build_tools(tree, jobs):
    work = BUILD / 'shader-toolchain'
    work.mkdir(parents=True, exist_ok=True)
    build_xlate.verify_vendor()
    identity = hashlib.sha256(build_xlate.identity().encode())
    identity.update((tree.parent.parent / '.source-id').read_bytes())
    source = tree / 'tools/totk_xlate'
    for p in sorted([*source.glob('*.cs'), source / 'totk_xlate.csproj', Path(__file__)]):
        identity.update(p.name.encode()); identity.update(p.read_bytes())
    identity.update(subprocess.check_output(['dotnet', '--version']))
    identity.update(SPIRV_PIN.encode())
    stamp = work / 'source.id'
    service = work / 'service/totk_xlate'
    if not service.is_file() or not stamp.is_file() or stamp.read_text() != identity.hexdigest():
        previous = build_xlate.BUILD
        try:
            build_xlate.BUILD = work
            vendor = build_xlate.prepare_sources()
            # Windows patch 0008 changes the support-buffer ABI to carry NVN
            # point-size state. The native renderer uses the original ABI.
            run(['git', 'apply', '--reverse', '--directory', 'ryujinx',
                 ROOT / 'tools/xlate/patches/0008-point-size-from-support-buffer.patch'], cwd=work)
        finally:
            build_xlate.BUILD = previous
        project = work / 'xlate'
        project.mkdir(exist_ok=True)
        for p in [*source.glob('*.cs'), source / 'totk_xlate.csproj']:
            shutil.copy2(p, project / p.name)
        # Homebrew's .NET NativeAOT pack uses system OpenSSL/Brotli. Expose only
        # their static archives so the app has no Homebrew runtime dependency.
        static = work / 'static-libs'
        static.mkdir(exist_ok=True)
        for formula, names in [('openssl@3', ('ssl', 'crypto')), ('brotli', ('brotlienc', 'brotlidec', 'brotlicommon'))]:
            for name in names:
                archive = f'lib{name}.a'
                prefix = library_prefix(formula, archive)
                dest = static / archive
                if dest.is_symlink(): dest.unlink()
                dest.symlink_to(prefix / 'lib' / archive)
        run(['dotnet', 'publish', project / 'totk_xlate.csproj', '-c', 'Release', '-r', 'osx-arm64',
             '-p:PublishAot=true', '-p:InvariantGlobalization=true', '-p:AppleMinOSVersion=26.0', f'-p:RyuSrc={vendor}',
             '-o', service.parent], env=dict(os.environ, MACOSX_DEPLOYMENT_TARGET='26.0', LIBRARY_PATH=str(static)))
        write(stamp, identity.hexdigest())
    # Include the actual native binary in the cache namespace as well as sources.
    run(['codesign', '--force', '--sign', '-', service])
    cache_id = hashlib.sha256((identity.hexdigest() + digest(service)).encode()).hexdigest()
    write(service.with_suffix('.id'), cache_id + '\n')
    link(tree / 'build/shader-service', service.parent)
    src = work / 'SPIRV-Cross'
    if not src.exists():
        run(['git', 'init', '-q', src])
        run(['git', '-C', src, 'remote', 'add', 'origin', 'https://github.com/KhronosGroup/SPIRV-Cross.git'])
    commit = subprocess.run(['git', '-C', src, 'rev-parse', 'HEAD'], capture_output=True, text=True)
    if commit.returncode or commit.stdout.strip() != SPIRV_PIN:
        run(['git', '-C', src, 'fetch', '--depth', '1', 'origin', SPIRV_PIN])
        run(['git', '-C', src, 'checkout', '--detach', SPIRV_PIN])
    # Consume only committed files, just as for the native runtime.
    if subprocess.check_output(['git', '-C', src, 'status', '--porcelain']):
        raise RuntimeError('SPIRV-Cross checkout has local modifications')
    out = work / 'spirv-cross'
    out.mkdir(exist_ok=True)
    binary = out / 'spirv-cross'
    flags = ['-arch', 'arm64', '-mmacosx-version-min=26.0', '-std=c++11', '-O3', '-DNDEBUG', '-Wno-deprecated-declarations']
    cross_identity = hashlib.sha256(json.dumps([SPIRV_PIN, SPIRV_SOURCES, flags,
        subprocess.check_output(['xcrun','clang++','--version'],text=True)], sort_keys=True).encode()).hexdigest()
    cross_stamp = out / 'build.id'
    if not binary.is_file() or not cross_stamp.is_file() or cross_stamp.read_text() != cross_identity:
        def compile_one(name):
            run(['xcrun', 'clang++', *flags, '-c', src / name, '-o', out / (name + '.o')])
        with ThreadPoolExecutor(jobs) as pool:
            list(pool.map(compile_one, SPIRV_SOURCES))
        run(['xcrun', 'clang++', '-arch', 'arm64', '-mmacosx-version-min=26.0',
             *[out / (name + '.o') for name in SPIRV_SOURCES], '-o', binary])
        write(cross_stamp, cross_identity)
    link(tree / 'build/spirv-cross', out)
    for executable in (service, binary):
        run(['lipo', '-verify_arch', 'arm64', executable])
        run(['codesign', '--force', '--sign', '-', executable])
        dependencies = subprocess.check_output(['otool', '-L', executable], text=True).splitlines()[1:]
        if any(not line.strip().startswith(('/usr/lib/', '/System/Library/')) for line in dependencies):
            raise RuntimeError(f'Native helper has an unbundled dependency: {executable}')
    cache_id = hashlib.sha256((identity.hexdigest() + digest(service) + digest(binary)).encode()).hexdigest()
    write(service.with_suffix('.id'), cache_id + '\n')
    return service, binary


def prepare_shaders(tree, version, jobs):
    service, cross = build_tools(tree, jobs)
    seed = tree / f'build/shader-seed/{version}'
    run([sys.executable, tree / 'scripts/prepare_shader_seed.py', '--romfs', tree / f'extracted/{version}/romfs',
         '--output', seed, '--translator', service, '--spirv-cross', cross, '--version', version,
         '--modules', tree / f'rewritten/{version}'], cwd=tree)
    run([sys.executable, tree / 'scripts/shader_list.py', 'export', tree / 'assets/shader_list.tsv',
         tree / 'build/pso_manifest.tsv', tree / 'build/pso_title.tsv'], cwd=tree)
    libraries = tree / 'build/shader-libs'
    libraries.mkdir(parents=True, exist_ok=True)
    def compile_shader(source):
        out = libraries / (digest(source) + '.metallib')
        run(['xcrun', 'metal', '-fmetal-math-mode=safe', '-o', out, source])
    with ThreadPoolExecutor(jobs) as pool:
        list(pool.map(compile_shader, sorted(seed.glob('*.metal'))))
    write(seed / 'translator.id', service.with_suffix('.id').read_text())


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--jobs', type=int, default=min(os.cpu_count() or 4, 12))
    a = ap.parse_args()
    if a.jobs < 1: ap.error('--jobs must be positive')
    BUILD.mkdir(parents=True, exist_ok=True)
    print(*build_tools(stage_sources(), a.jobs), sep='\n')


if __name__ == '__main__': main()
