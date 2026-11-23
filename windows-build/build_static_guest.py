"""Build the Windows Static Recomp game library locally from a Melee NTSC 1.02 disc."""
import argparse
import concurrent.futures
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DOL_SHA256 = 'dc21504513424350bda17a7c65e82371b45112a5dfc1e9f2749a8b7ab0eff646'


def build(args, work):
    if args.iso:
        dol = work / 'main.dol'
        subprocess.run([sys.executable, ROOT / 'tools/extract_dol.py', args.iso, dol], check=True)
    else:
        dol = args.dol
    if hashlib.sha256(dol.read_bytes()).hexdigest() != DOL_SHA256:
        raise SystemExit('Use an unmodified Melee NTSC 1.02 disc or its extracted main.dol')

    generated = work / 'translation'
    subprocess.run([sys.executable, ROOT / 'port/recomp/recomp.py', '--dol', dol,
                    '--gct-base', '0x8065CC80', '--out', generated], check=True)
    sys.path.insert(0, str(ROOT / 'port/recomp'))
    from gecko import generate_gct, load_ini
    expected = generate_gct(load_ini(ROOT / 'port/slippi_sys/GameSettings/GALE01r2.ini'))[0]
    code = (generated / 'gecko_data.cpp').read_text()
    table = re.search(r'const uint8_t slippi_gct\[\d+\] = \{(.*?)\};', code, re.S)
    if not table or bytes.fromhex(''.join(re.findall(r'0x([\da-fA-F]{2})', table[1]))) != expected:
        raise SystemExit('Generated Slippi table differs from the current checkout')

    compiler = [args.clang, '--driver-mode=cl', '--target=x86_64-pc-windows-msvc',
                '/nologo', '/std:c++17', '/MD', '/O2', '/EHsc', '/fp:precise',
                '/bigobj', '/W0', '/DNOMINMAX', '/D_CRT_SECURE_NO_WARNINGS',
                '/I' + str(generated), '/I' + str(ROOT / 'port/runtime/ppc'),
                '/I' + str(ROOT / 'port/runtime/hle')]
    if args.msvc_sysroot:
        # xwin's "splat" layout, with the genuine Microsoft CRT and Windows SDK headers.
        for folder in ['crt/include', 'sdk/include/ucrt', 'sdk/include/shared', 'sdk/include/um']:
            include = args.msvc_sysroot / folder
            if not include.is_dir():
                raise SystemExit(f'Missing MSVC or Windows SDK headers: {include}')
            compiler += ['/imsvc' + str(include)]
    elif os.name != 'nt':
        raise SystemExit('--msvc-sysroot must point at an xwin splat when cross-compiling')

    objects = work / 'objects'
    objects.mkdir(exist_ok=True)
    sources = sorted(generated.glob('guest_*.cpp')) + [generated/'function_names.cpp', generated/'gecko_data.cpp']
    if not (generated / 'guest_table.cpp').is_file() or len(sources) < 4:
        raise SystemExit('Recompiler did not produce the guest and dispatch table')

    def compile_one(source):
        obj = objects / (source.stem + '.obj')
        subprocess.run([*compiler, '/c', '/Fo' + str(obj), '--', str(source)], check=True)
        return obj

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as workers:
        objs = list(workers.map(compile_one, sources))
    # Use the x64 baseline ISA (SSE2) in the guest. Both host baselines can link this library.
    lib = work / 'guest.lib'
    subprocess.run([args.librarian, '/nologo', '/machine:x64', '/out:' + str(lib), *objs], check=True)
    if expected not in lib.read_bytes():
        raise SystemExit('The compiled library is missing the current Slippi table')
    args.output.parent.mkdir(exist_ok=True, parents=True)
    shutil.copyfile(lib, args.output)
    print(f'{hashlib.sha256(args.output.read_bytes()).hexdigest()}  {args.output}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument('--iso', type=Path)
    inputs.add_argument('--dol', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--clang', default='clang', help='LLVM clang executable supporting the MSVC target')
    parser.add_argument('--librarian', default='llvm-lib', help='LLVM COFF librarian executable')
    parser.add_argument('--msvc-sysroot', type=Path, help='Microsoft toolchain headers in xwin splat layout')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    with tempfile.TemporaryDirectory(prefix='melee-static-guest-') as directory:
        build(args, Path(directory))
