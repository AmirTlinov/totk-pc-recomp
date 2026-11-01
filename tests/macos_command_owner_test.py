"""Exercise the patched production NVN recorder without game data or a GPU."""
from pathlib import Path
import argparse
import io
import os
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from build_xlate import mng_pin


def section(source, start, end):
    assert source.count(start) == 1, start
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='native-command-owner-', dir=ROOT / 'build') as temporary:
        work = Path(temporary)
        if args.source_root:
            nvn = args.source_root
        else:
            # Export only source files in an isolated directory. This test must
            # never replace the source tree of a concurrent game build.
            archive = subprocess.check_output(['git', '-C', ROOT / 'refs/MacNativeGames',
                'archive', mng_pin(), 'games/totk/third_party/d2r_gfx/gfx/nvn'])
            with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
                tar.extractall(work, filter='data')
            for patch in sorted((ROOT / 'runtime/macos/patches').glob('*.patch')):
                subprocess.run(['git', 'apply', '--unsafe-paths', '--directory', str(work / 'games/totk'),
                    '--include=*hle_nvn.c', '--include=*nvn_obj.h', '--include=*/include/nvn.h',
                    '--include=*nvn_command_owner.h', patch], cwd=ROOT, check=True)
            nvn = work / 'games/totk/third_party/d2r_gfx/gfx/nvn'
        source = (nvn / 'hle_nvn.c').read_text()
        chunks = [
            section(source, '#define CB(c)', '// TOTK diagnostics: guest return chain'),
            section(source, 'static void release_copies(uintptr_t lo, uintptr_t hi);',
                    '// TOTK: recordings keep texture objects'),
            section(source, '#define PIN_SWEEP_SUBMITS', 'NVN(nvnCommandBufferSetDepthRange'),
            section(source, 'static _Atomic int g_def_on', '// 图像（可写贴图）'),
        ]
        (work / 'recorder.inc').write_text('\n'.join(chunks))
        binary = work / 'command-owner'
        env = dict(os.environ)
        for key in list(env):
            if key.startswith(('TOTK_', 'DM_NVN_')):
                env.pop(key)
        includes = ['-I', work, '-I', nvn, '-I', nvn / 'include', '-I', ROOT / 'runtime/macos']
        if args.source_root:
            exported = ROOT / 'build/macos-arm64/source/games/totk/third_party/d2r_gfx/gfx/nvn'
            includes += ['-I', exported, '-I', exported / 'include']
        subprocess.run(['xcrun', 'clang', '-std=gnu11', '-O1', '-g',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
            '-Wno-format', '-Wno-unused-function', '-Wno-unused-parameter',
            *includes,
            ROOT / 'tests/macos_command_owner_test.c', '-o', binary], check=True)
        subprocess.run([binary], env=env, check=True, timeout=60)


if __name__ == '__main__':
    main()
