"""Package a native local app; player data is never part of a public release."""
import json
import plistlib
import shutil
import subprocess
from pathlib import Path

from macos.build import ROOT, INTEGRATION, BUILD, run, digest, library_prefix


def copy(source, destination):
    source, destination = Path(source), Path(destination)
    if not source.exists():
        raise RuntimeError(f'Missing application input: {source}')
    destination.parent.mkdir(parents=True, exist_ok=True)
    if source.is_dir():
        # APFS clones keep immutable package trees cheap even when they contain
        # tens of thousands of shader files.  copytree remains the fallback for
        # another filesystem or an older host.
        clone_source = source.resolve() if source.is_symlink() else source
        if subprocess.run(['cp', '-cR', str(clone_source), str(destination)], capture_output=True).returncode:
            shutil.copytree(source, destination)
    elif source.stat().st_size >= 64 * (1 << 20):
        # clonefile through cp keeps local multi-GB game copies cheap on APFS.
        if subprocess.run(['cp', '-c', str(source), str(destination)], capture_output=True).returncode:
            shutil.copy2(source, destination)
    else:
        # Shader libraries are small and numerous; avoid a process per file.
        shutil.copy2(source, destination)


def game_data_receipt(tree, version):
    romfs = tree / f'extracted/{version}/romfs/romfs.bin'
    index = tree / f'extracted/{version}/romfs/runtime-index.bin'
    verified = json.loads((tree / f'extracted/{version}/romfs/index.json').read_text())
    if verified.get('status') != 'pass' or not verified.get('romfs_sha256'):
        raise RuntimeError('Verified RomFS receipt is missing')
    stat = romfs.stat()
    return {
        'version': version,
        'romfs_source': {'device': stat.st_dev, 'inode': stat.st_ino,
                         'size': stat.st_size, 'mtime_ns': stat.st_mtime_ns},
        'romfs_sha256': verified['romfs_sha256'],
        'runtime_index_size': index.stat().st_size,
        'runtime_index_sha256': digest(index),
    }


def game_data_current(data, receipt):
    proof = data / 'receipt.json'
    romfs = data / f'extracted/{receipt["version"]}/romfs/romfs.bin'
    index = data / f'extracted/{receipt["version"]}/romfs/runtime-index.bin'
    if not romfs.is_file() or not index.is_file():
        return False
    if not proof.is_file():
        # Adopt packages made before the external-data receipt existed.  This
        # is the only pass that hashes the large clone; later packaging keeps
        # its inode stable so the launcher's verified hash cache remains hot.
        if (romfs.stat().st_size != receipt['romfs_source']['size'] or
                digest(romfs) != receipt['romfs_sha256'] or
                index.stat().st_size != receipt['runtime_index_size'] or
                digest(index) != receipt['runtime_index_sha256']):
            return False
        proof.write_text(json.dumps(receipt, indent=2) + '\n')
        return True
    try:
        saved = json.loads(proof.read_text())
    except (OSError, json.JSONDecodeError):
        return False
    return (saved == receipt and romfs.stat().st_size == receipt['romfs_source']['size'] and
            index.stat().st_size == receipt['runtime_index_size'] and
            digest(index) == receipt['runtime_index_sha256'])


def shader_data_receipt(tree, version):
    seed = tree / f'build/shader-seed/{version}'
    manifest_path = seed / 'seed-manifest.json'
    manifest = json.loads(manifest_path.read_text())
    translator = (seed / 'translator.id').read_text().strip()
    expected_translator = (tree / 'build/shader-service/totk_xlate.id').read_text().strip()
    libraries = tree / 'build/shader-libs'
    hashes = sorted({sha for name, sha in manifest.get('files', {}).items() if name.endswith('.metal')})
    if (manifest.get('version') != version or manifest.get('shaders', 0) < 8000 or not hashes or
            translator != expected_translator or not (seed / 'preparation.id').is_file()):
        raise RuntimeError('Prepared shader seed identity failed')
    return ({
        'schema': 1,
        'version': version,
        'manifest_sha256': digest(manifest_path),
        'preparation_id': (seed / 'preparation.id').read_text().strip(),
        'translator_id': translator,
        'compiler_id': libraries.resolve().name,
        'seed_files': len(manifest['files']),
        'libraries': len(hashes),
    }, manifest, hashes)


def shader_data_current(target, receipt, hashes):
    proof = target / 'receipt.json'
    seed = target / 'seed'
    libraries = target / 'libraries'
    try:
        saved = json.loads(proof.read_text())
    except (OSError, json.JSONDecodeError):
        return False
    manifest = seed / 'seed-manifest.json'
    if (saved != receipt or not manifest.is_file() or not libraries.is_dir() or
            digest(manifest) != receipt['manifest_sha256']):
        return False
    # scandir and file existence are metadata-only.  Complete content was
    # checked before this immutable generation was published.
    if sum(entry.is_file() for entry in seed.iterdir()) < receipt['seed_files'] + 3:
        return False
    return all((libraries / (sha + '.metallib')).is_file() for sha in hashes)


def validate_shader_sources(tree, version, manifest, hashes):
    seed = tree / f'build/shader-seed/{version}'
    if any(Path(name).name != name or digest(seed / name) != sha
           for name, sha in manifest['files'].items()):
        raise RuntimeError('Prepared shader seed integrity failed')
    libraries = tree / 'build/shader-libs'
    if any(not (libraries / (sha + '.metallib')).is_file() for sha in hashes):
        raise RuntimeError('A prepared Metal shader library is missing')


def package(tree, out, version, probes):
    app = out / ('TotK Recomp Diagnostics.app' if probes else 'TotK Recomp.app')
    stage = app.with_suffix('.app.partial')
    data = out / 'TotK Recomp Data'
    data_stage = out / 'TotK Recomp Data.partial'
    data_replace = False
    shader_replace = False
    shader_stage = shader_target = None
    if stage.exists():
        shutil.rmtree(stage)
    macos = stage / 'Contents/MacOS'
    resources = stage / 'Contents/Resources'
    root = resources / 'totk'
    macos.mkdir(parents=True)
    root.mkdir(parents=True)
    copy(out / 'totk-native', macos / 'totk-native')
    copy(out / 'build-info.json', root / 'build-info.json')
    copy(ROOT / 'THIRD_PARTY_NOTICES.md', resources / 'THIRD_PARTY_NOTICES.md')
    copy(tree.parent.parent / 'LICENSE', resources / 'MacNativeGames-LICENSE')
    copy(tree / 'third_party/d2r_gfx/LICENSE', resources / 'd2r-LICENSE')
    copy(tree / 'third_party/d2r_gfx/LICENSE.deko3d', resources / 'deko3d-LICENSE')
    copy(tree / 'third_party/licenses', resources / 'licenses/upstream')
    copy(INTEGRATION / 'licenses/libvpx-PATENTS.txt', resources / 'licenses/libvpx-PATENTS.txt')
    for formula, archive, name in [('opus','libopus.a','COPYING'), ('libvpx','libvpx.a','LICENSE')]:
        prefix=library_prefix(formula,archive)
        copy(prefix / name, resources / 'licenses' / f'{formula}-{name}')
    if not probes:
        receipt = game_data_receipt(tree, version)
        if not game_data_current(data, receipt):
            if data_stage.exists():
                shutil.rmtree(data_stage)
            romfs = data_stage / f'extracted/{version}/romfs'
            copy(tree / f'extracted/{version}/romfs/romfs.bin', romfs / 'romfs.bin')
            copy(tree / f'extracted/{version}/romfs/runtime-index.bin', romfs / 'runtime-index.bin')
            (data_stage / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
            data_replace = True
        shader_receipt, shader_manifest, shader_hashes = shader_data_receipt(tree, version)
        shader_base = data_stage if data_replace else data
        shader_target = shader_base / 'shaders' / version
        shader_stage = shader_target.with_name(version + '.partial')
        if not shader_data_current(shader_target, shader_receipt, shader_hashes):
            validate_shader_sources(tree, version, shader_manifest, shader_hashes)
            if shader_stage.exists():
                shutil.rmtree(shader_stage)
            copy(tree / f'build/shader-seed/{version}', shader_stage / 'seed')
            copy(tree / 'build/shader-libs', shader_stage / 'libraries')
            (shader_stage / 'receipt.json').write_text(json.dumps(shader_receipt, indent=2) + '\n')
            shader_replace = True
        copy(ROOT / 'tools/vendor/ryujinx/LICENSE.txt', resources / 'licenses/Ryujinx-LICENSE.txt')
        copy(BUILD / 'shader-toolchain/SPIRV-Cross/LICENSE', resources / 'licenses/SPIRV-Cross-LICENSE')
        dotnet=Path(subprocess.check_output(['brew','--prefix','dotnet'],text=True).strip())
        for name in ('LICENSE.txt','ThirdPartyNotices.txt'):
            copy(dotnet / 'share/doc/dotnet' / name, resources / 'licenses' / ('dotnet-'+name))
        for formula, archive, name in [('openssl@3','libssl.a','LICENSE.txt'), ('brotli','libbrotlicommon.a','LICENSE')]:
            prefix=library_prefix(formula,archive)
            copy(prefix / name, resources / 'licenses' / f'{formula}-{name}')
        files = [*[f'rewritten/{version}/{name}.patched.bin' for name in ('main','subsdk0','sdk')],
                 f'extracted/{version}/romfs-patches',
                 f'extracted/{version}/romfs-patches-out', 'extracted/firmware/shared-fonts',
                 'build/spirv-cross/spirv-cross', 'build/shader-service/totk_xlate',
                 'build/shader-service/totk_xlate.id', 'build/pso_manifest.tsv', 'build/pso_title.tsv']
        for name in files:
            copy(tree / name, root / name)
    run(['xcrun', 'clang', '-arch', 'arm64', '-mmacosx-version-min=26.0', '-fobjc-arc', '-O2',
         '-Wall', '-Wextra', INTEGRATION / 'launcher.m', '-framework', 'AppKit', '-framework', 'Metal',
         '-o', macos / 'TotKRecomp'])
    info = dict(CFBundleExecutable='TotKRecomp', CFBundleIdentifier='org.totkrecomp.mac.diagnostics' if probes else 'org.totkrecomp.mac',
                CFBundleName='TotK Recomp', CFBundlePackageType='APPL', CFBundleShortVersionString='0.1.0',
                CFBundleVersion='1', LSMinimumSystemVersion='26.0', NSHighResolutionCapable=True,
                LSSupportsGameMode=True, LSApplicationCategoryType='public.app-category.games',
                GCSupportsControllerUserInteraction=True)
    icon = BUILD / 'AppIcon.icns'
    if icon.is_file():
        copy(icon, resources / 'AppIcon.icns')
        info['CFBundleIconFile'] = 'AppIcon'
    (stage / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
    # The bundle signature also signs its main launcher. Signing that path
    # separately would seal and hash all game resources a second time.
    run(['codesign', '--force', '--sign', '-', macos / 'totk-native'])
    run(['codesign', '--force', '--sign', '-', stage])
    run(['codesign', '--verify', '--deep', '--strict', stage])
    run([macos / 'TotKRecomp', '--check'])
    if app.exists():
        # Do not replace files underneath a running game.
        commands = subprocess.check_output(['ps', '-axo', 'comm='], text=True).splitlines()
        if str(app / 'Contents/MacOS/totk-native') in (p.strip() for p in commands):
            raise RuntimeError(f'The replacement is ready at {stage}; close the game before packaging again')
        shutil.rmtree(app)
    if shader_replace:
        if shader_target.exists():
            shutil.rmtree(shader_target)
        shader_stage.rename(shader_target)
    if data_replace:
        if data.exists():
            shutil.rmtree(data)
        data_stage.rename(data)
    stage.rename(app)
    return app
