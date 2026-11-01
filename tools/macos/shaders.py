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
import tempfile

import build_xlate
from macos.build import BUILD, ROOT, digest, run, stage_sources, write, library_prefix
from macos.prepare import link

SPIRV_PIN = '98cbb7c02be35364ecacc23dc0c038d2360b0ed0'
SERVICE_BUILD_SCHEMA = 'totk-xlate-nativeaot-macos26-v1'
METAL_FLAGS = ('-std=metal3.2', '-fmetal-math-mode=safe', '-Werror=sometimes-uninitialized')
SPIRV_SOURCES = ('main.cpp spirv_cfg.cpp spirv_cpp.cpp spirv_cross.cpp spirv_cross_parsed_ir.cpp '
                 'spirv_cross_util.cpp spirv_glsl.cpp spirv_hlsl.cpp spirv_msl.cpp spirv_parser.cpp spirv_reflect.cpp').split()


def metal_version():
    output = subprocess.check_output(['xcrun', 'metal', '--version'], text=True)
    # Xcode mounts the Metal toolchain under a temporary Cryptex directory.
    # Its location can change after reboot without changing the compiler.
    version = '\n'.join(line for line in output.splitlines() if not line.startswith('InstalledDir:')).strip()
    if not version:
        raise RuntimeError('Metal compiler did not report a version')
    return version


def metal_compiler_identity():
    return hashlib.sha256(json.dumps([METAL_FLAGS, metal_version()]).encode()).hexdigest()


def shader_translation_list_digest(path):
    """Hash only shader variants and locators, which are the translation inputs."""
    rows = []
    for line in Path(path).read_bytes().splitlines():
        if line.startswith((b'V\t', b'L\t')):
            rows.append(line)
    return hashlib.sha256(b'\n'.join(rows)).hexdigest()


def shader_preparation_identity(tree, version, service, cross):
    """Hash shader-only inputs without coupling them to the game lifter."""
    tree = Path(tree).resolve()
    roots = [
        tree / 'assets/shader_list.tsv',
        tree / 'scripts/prepare_shader_seed.py',
        tree / 'scripts/shader_list.py',
        tree / 'third_party/d2r_gfx/gfx/nvn/dmsh_chain.h',
        tree / f'rewritten/{version}/provenance.json',
        tree / f'extracted/{version}/romfs/index.json',
        service,
        cross,
    ]
    identity = hashlib.sha256(f'macos-shader-preparation-v2\0{version}\0'.encode())
    for path in roots:
        if not path.is_file():
            raise RuntimeError(f'Missing shader preparation input: {path}')
        try:
            name = path.relative_to(ROOT)
        except ValueError:
            name = path.relative_to(tree)
        identity.update(str(name).encode())
        identity.update(b'\0')
        content_digest = (shader_translation_list_digest(path)
                          if path == tree / 'assets/shader_list.tsv' else digest(path))
        identity.update(bytes.fromhex(content_digest))
    return identity.hexdigest()


def build_tools(tree, jobs):
    work = BUILD / 'shader-toolchain'
    work.mkdir(parents=True, exist_ok=True)
    build_xlate.verify_vendor()
    identity = hashlib.sha256(build_xlate.identity().encode())
    source = tree / 'tools/totk_xlate'
    identity.update(SERVICE_BUILD_SCHEMA.encode())
    for p in sorted([*source.glob('*.cs'), source / 'totk_xlate.csproj']):
        identity.update(p.name.encode()); identity.update(p.read_bytes())
    identity.update(subprocess.check_output(['dotnet', '--version']))
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
        run(['codesign', '--force', '--sign', '-', service])
        write(stamp, identity.hexdigest())
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
    # APFS is usually case-insensitive: "spirv-cross" aliases "SPIRV-Cross".
    out = work / 'spirv-cross-build'
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
        run(['codesign', '--force', '--sign', '-', binary])
        write(cross_stamp, cross_identity)
    link(tree / 'build/spirv-cross', out)
    for executable in (service, binary):
        run(['lipo', '-verify_arch', 'arm64', executable])
        dependencies = subprocess.check_output(['otool', '-L', executable], text=True).splitlines()[1:]
        if any(not line.strip().startswith(('/usr/lib/', '/System/Library/')) for line in dependencies):
            raise RuntimeError(f'Native helper has an unbundled dependency: {executable}')
    metal = metal_version()
    # Runtime shader keys follow executable translator behaviour. Python build
    # orchestration must not invalidate tens of thousands of shaders when the
    # produced translator and SPIRV-Cross binaries are byte-identical.
    cache_id = hashlib.sha256((digest(service) + digest(binary) + metal).encode()).hexdigest()
    write(service.with_suffix('.id'), cache_id + '\n')
    return service, binary


def compile_libraries(sources, libraries, jobs):
    """Compile each distinct MSL once; publish only complete cache entries."""
    libraries.mkdir(parents=True, exist_ok=True)
    def compile_shader(item):
        sha, source = item
        out = libraries / (sha + '.metallib')
        if out.is_file() and out.stat().st_size:
            return 0
        with tempfile.TemporaryDirectory(dir=libraries, prefix='compile-') as temporary:
            compiled = Path(temporary) / 'shader.metallib'
            run(['xcrun', 'metal', *METAL_FLAGS, '-o', compiled, source])
            compiled.replace(out)
        return 1
    unique = dict(sources)
    with ThreadPoolExecutor(jobs) as pool:
        return sum(pool.map(compile_shader, unique.items()))


def prepare_shaders(tree, version, jobs):
    service, cross = build_tools(tree, jobs)
    # Runtime source exports may be replaced; immutable compiled assets survive.
    seeds = BUILD / 'shader-seed'
    seeds.mkdir(exist_ok=True)
    link(tree / 'build/shader-seed', seeds)
    seed = tree / f'build/shader-seed/{version}'
    preparation_id = shader_preparation_identity(tree, version, service, cross)
    stamp = seed / 'preparation.id'
    current = stamp.is_file() and stamp.read_text().strip() == preparation_id
    if not current:
        run([sys.executable, tree / 'scripts/prepare_shader_seed.py', '--romfs', tree / f'extracted/{version}/romfs',
             '--output', seed, '--translator', service, '--spirv-cross', cross, '--version', version,
             '--modules', tree / f'rewritten/{version}'], cwd=tree,
            env=dict(os.environ, TOTK_SHADER_DEBUGMODE='1'))
    warm_id = digest(tree / 'assets/shader_list.tsv')
    warm_stamp = tree / 'build/pso_manifest.id'
    if (not warm_stamp.is_file() or warm_stamp.read_text().strip() != warm_id or
            not (tree / 'build/pso_manifest.tsv').is_file() or not (tree / 'build/pso_title.tsv').is_file()):
        run([sys.executable, tree / 'scripts/shader_list.py', 'export', tree / 'assets/shader_list.tsv',
             tree / 'build/pso_manifest.tsv', tree / 'build/pso_title.tsv'], cwd=tree)
        write(warm_stamp, warm_id + '\n')
    manifest_path = seed / 'seed-manifest.json'
    if not manifest_path.is_file():
        raise RuntimeError('Shader preparation did not produce a manifest')
    manifest = json.loads(manifest_path.read_text())
    if (manifest.get('version') != version or manifest.get('shaders', 0) < 8000 or
            manifest.get('chain') not in (tree / 'third_party/d2r_gfx/gfx/nvn/dmsh_chain.h').read_text()):
        raise RuntimeError('Prepared shader seed has the wrong identity')
    # Entries already include the full MSL hash. A translator change that emits
    # identical MSL must not recompile it; only compiler options/version matter.
    compiler_id = metal_compiler_identity()
    libraries = BUILD / 'shader-libs' / compiler_id
    libraries.mkdir(parents=True, exist_ok=True)
    link(tree / 'build/shader-libs', libraries)
    sources = [(sha, seed / name) for name, sha in manifest['files'].items() if name.endswith('.metal')]
    if any(not source.is_file() for _, source in sources):
        raise RuntimeError('Prepared shader seed is incomplete')
    compile_libraries(sources, libraries, jobs)
    write(seed / 'translator.id', service.with_suffix('.id').read_text())
    write(stamp, preparation_id + '\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--jobs', type=int, default=min(os.cpu_count() or 4, 12))
    a = ap.parse_args()
    if a.jobs < 1: ap.error('--jobs must be positive')
    BUILD.mkdir(parents=True, exist_ok=True)
    print(*build_tools(stage_sources(), a.jobs), sep='\n')


if __name__ == '__main__': main()
