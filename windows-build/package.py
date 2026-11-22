"""Assemble a Source Port tester zip from the Windows build and pinned runtime assets."""
from pathlib import Path, PurePosixPath
import argparse
import zipfile

parser = argparse.ArgumentParser()
for name in ('host', 'game', 'assets', 'out'):
    parser.add_argument('--' + name, required=True, type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
folder = 'Melee-Unlocked-Windows/'
directories = {'Sys', 'SysPlayback', 'ProAI', 'Shaders', 'ui_sources', 'deltas', 'lang', 'licenses', 'tools'}
libraries = {'librashader.dll', 'libxess.dll', 'dxcompiler.dll', 'dxil.dll', 'onnxruntime.dll'}
bat = r'''@echo off
setlocal
cd /d "%~dp0"
set "DISC=%~1"
if not defined DISC set "DISC=%~dp0melee.iso"
if not exist "%DISC%" (
  echo Drag your Melee NTSC 1.02 ISO onto Play.bat, or place melee.iso beside it.
  pause
  exit /b 1
)
melee_source.exe --iso "%DISC%" --pc-settings --slippi-menus on --sys-dir Sys --user-dir "User\Slippi" --card-dir "User\GC\CardA" --settings-path port-settings.ini --replay-dir Replays --threaded-renderer
if errorlevel 1 pause
'''
with zipfile.ZipFile(args.out, 'w', zipfile.ZIP_DEFLATED) as z, zipfile.ZipFile(args.assets) as assets:
    copied = set()
    for item in assets.infolist():
        name = PurePosixPath(item.filename)
        if item.is_dir() or name.parts[0] != 'MeleeUnlocked-1.0.5' or '..' in name.parts: continue
        path = PurePosixPath(*name.parts[1:])
        if path.parts[0] in directories or (len(path.parts)==1 and (path.name in libraries or path.name.startswith(('vcruntime','msvcp','concrt','vccorlib')) and path.suffix=='.dll')):
            z.writestr(folder+str(path),assets.read(item))
            copied.add(str(path))
    assert 'Sys/GC/dsp_coef.bin' in copied
    for p in [args.host/'melee_source.exe',args.game/'melee_game.dll',args.game/'melee_game.snapexcl',root/'LICENSE',root/'NOTICE',*args.host.glob('*.dxil'),*args.host.glob('*.dll')]:
        if p.name not in copied:
            z.write(p,folder+p.name)
            copied.add(p.name)
    assert 'dxr_pathtrace.dxil' in copied
    z.writestr(folder+'Play.bat',bat.replace('\n','\r\n'))
