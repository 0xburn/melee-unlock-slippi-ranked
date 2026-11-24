"""Prepare pinned upstream dependencies and run the project's Windows release packager."""
import argparse
import hashlib
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = 'MeleeUnlocked-1.0.22'
REPO = 'Hero88go/melee-unlocked'
# These hashes pin the inputs approved for this upstream revision. An upstream
# tag or asset replacement must never silently change a rebuild of the fork.
ASSETS = {
    UPSTREAM + '-win64.zip': ('v1.0.22', '38698ed2ab32be2a45f3c9ee6c333f6965fc150eb707e7b9f5f242e5bd60fd1f'),
    UPSTREAM + '-RTX-Pack.zip': ('parts-1.0.22', 'f1c8248936560aefafe2fed6dbcef030334c89b73824e2a307544cfd241f9cb6'),
    'MeleeUnlocked-XeSS.zip': ('v0.9.98', '2b170a5334c80d843afb7bc002e186991155d79f4a6d54131d1f762d96d5229e'),
    'MeleeUnlocked-DLSS5-Model.zip': ('v0.9.57', '2b5753872c281ec65276dda263f80e510b04328e33ff7d85fc4adba063a74fb7'),
    'MeleeUnlocked-Lab.zip': ('content-1', '6fcb60586ee0b73a42b3b7521546b554d3b105299bc97d74720b849075febb75'),
}


def pinned_asset(cache, name):
    tag, digest = ASSETS[name]
    path = cache / name
    if not path.exists():
        cache.mkdir(parents=True, exist_ok=True)
        subprocess.run(['gh', 'release', 'download', tag, '--repo', REPO, '--pattern', name,
                        '--dir', str(cache)], check=True)
    with path.open('rb') as f:
        if hashlib.file_digest(f, 'sha256').hexdigest() != digest:
            raise ValueError(f'Upstream asset changed: {name}')
    return path


def copy_member(archive, member, dest):
    dest.parent.mkdir(parents=True, exist_ok=True)
    with archive.open(member) as source, dest.open('wb') as output:
        shutil.copyfileobj(source, output)


def manifest(path, folder):
    with zipfile.ZipFile(path) as archive:
        names = [PurePosixPath(i.filename).relative_to(folder).as_posix()
                 for i in archive.infolist() if not i.is_dir()]
    if len(set(names)) != len(names):
        raise ValueError(f'Duplicate release paths in {path}')
    return set(names)


def check_parity(reference, built, folder):
    expected = manifest(reference, UPSTREAM)
    expected.remove('yougame-native.json')
    expected.remove('shadercache/recipes.bin')
    expected.add('shadercache/recipes.shipped.bin')
    actual = manifest(built, folder)
    if actual != expected:
        raise ValueError(f'Review release changes. Missing: {sorted(expected-actual)}. Added: {sorted(actual-expected)}')


def package(args):
    cache = args.cache.resolve()
    base = pinned_asset(cache, UPSTREAM + '-win64.zip')
    host = args.exe.resolve().parent
    with zipfile.ZipFile(base) as archive:
        # Only supply missing build inputs to the real packager. Nothing from
        # this archive is copied directly into the release output directory.
        for name in ['nvngx.dll_meleedlss5.dll', 'onnxruntime.dll', 'librashader.dll']:
            copy_member(archive, UPSTREAM+'/'+name, host/name)
        for name, dest in [('shadercache/recipes.bin', ROOT/'shadercache/recipes.bin'),
                           ('tools/7zr.exe', ROOT/'port/third_party/mod_tools/7zr.exe'),
                           ('tools/xdelta3.exe', ROOT/'port/third_party/mod_tools/xdelta3.exe')]:
            copy_member(archive, UPSTREAM+'/'+name, dest)
        for item in archive.namelist():
            if item.startswith(UPSTREAM+'/ProAI/') and not item.endswith('/'):
                relative = PurePosixPath(item).relative_to(UPSTREAM+'/ProAI')
                if '..' in relative.parts:
                    raise ValueError(item)
                copy_member(archive, item, cache/'ProAI'/relative)
    lab = pinned_asset(cache, 'MeleeUnlocked-Lab.zip')
    (ROOT/'release/content-1').mkdir(parents=True, exist_ok=True)
    shutil.copyfile(lab, ROOT/'release/content-1'/lab.name)
    with zipfile.ZipFile(lab) as archive:
        for number in range(27):
            name = f'{number}.lab'
            copy_member(archive, name, ROOT/'build/lab'/name)
    for asset, member, dest in [('MeleeUnlocked-XeSS.zip', 'libxess.dll', host/'libxess.dll'),
                               ('MeleeUnlocked-DLSS5-Model.zip', 'nvngx_dlssnr.dll', ROOT/'port/third_party/ngx/dlssnr/nvngx_dlssnr.dll')]:
        with zipfile.ZipFile(pinned_asset(cache, asset)) as archive:
            copy_member(archive, member, dest)
    subprocess.run([sys.executable, str(ROOT/'tools/package_release.py'),
                    '--exe', args.exe, '--compat-exe', args.compat_exe, '--source-exe', args.source_exe,
                    '--source-compat-exe', args.source_compat_exe, '--source-dll', args.source_dll,
                    '--source-compat-dll', args.source_compat_dll, '--pro-ai-dir', cache/'ProAI',
                    '--gct-ini', ROOT/'port/slippi_sys/GameSettings/GALE01r2.ini',
                    '--rtx-pack', pinned_asset(cache, UPSTREAM+'-RTX-Pack.zip'), '--out', args.out], check=True)
    folder = 'MeleeUnlocked-' + (ROOT/'VERSION').read_text().strip()
    check_parity(base, args.out/(folder+'-win64.zip'), folder)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ('exe', 'compat-exe', 'source-exe', 'source-compat-exe',
                'source-dll', 'source-compat-dll', 'cache', 'out'):
        parser.add_argument('--'+arg, required=True, type=Path)
    package(parser.parse_args())
