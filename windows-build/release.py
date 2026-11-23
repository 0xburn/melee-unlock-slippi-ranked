"""Build Windows release archives from a clean, pushed fork checkout."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]


def output(*cmd):
    return subprocess.check_output(cmd, cwd=ROOT, text=True).strip()


def build(args, directory):
    sha = output('git', 'rev-parse', 'HEAD')
    branch = output('git', 'branch', '--show-current')
    if not branch or output('git', 'status', '--porcelain', '--untracked-files=no'):
        raise SystemExit('Commit release changes before building')
    repo = output('gh', 'repo', 'view', '--json', 'nameWithOwner', '--jq', '.nameWithOwner')
    if output('gh', 'api', f'repos/{repo}/commits/{branch}', '--jq', '.sha') != sha:
        raise SystemExit('Push this revision before building')
    guest = directory / 'guest.lib'
    cmd = [sys.executable, ROOT/'windows-build/build_static_guest.py', '--iso', args.iso.resolve(),
           '--output', guest, '--clang', args.clang, '--librarian', args.librarian]
    if args.msvc_sysroot:
        cmd += ['--msvc-sysroot', args.msvc_sysroot.resolve()]
    subprocess.run(cmd, cwd=ROOT, check=True)
    with guest.open('rb') as f:
        digest = hashlib.file_digest(f, 'sha256').hexdigest()
    tag = 'build-input-' + uuid.uuid4().hex
    def gh(*cmd):
        return output('gh', *cmd, '--repo', repo)
    gh('release', 'create', tag, '--draft', '--target', sha, '--title', 'Windows build input', '--notes', '')
    try:
        if gh('release', 'view', tag, '--json', 'isDraft', '--jq', '.isDraft') != 'true':
            raise RuntimeError('Private build input must be a draft')
        gh('release', 'upload', tag, str(guest))
        print(gh('workflow','run','windows-release.yml','--ref',branch,'-f','guest_release='+tag,'-f','guest_sha256='+digest), flush=True)
        for _ in range(36):
            runs = json.loads(gh('run','list','-w','windows-release.yml','-L','50','--json','databaseId,displayTitle,headSha'))
            run = next((r for r in runs if r['headSha']==sha and r['displayTitle']=='Windows '+tag), None)
            if run:
                break
            time.sleep(5)
        else:
            raise RuntimeError('Could not identify the dispatched Windows build')
        run_id = str(run['databaseId'])
        print(f'https://github.com/{repo}/actions/runs/{run_id}', flush=True)
        subprocess.run(['gh','run','watch',run_id,'--repo',repo,'--exit-status'],check=True,cwd=ROOT)
        gh('run','download',run_id,'-n','Melee-Unlocked-Windows-x64','-D',str(args.out.resolve()))
    finally:
        if gh('release', 'view', tag, '--json', 'isDraft', '--jq', '.isDraft') == 'true':
            gh('release','delete',tag,'--yes')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--out', type=Path, default=Path('release'))
    parser.add_argument('--clang', default='clang')
    parser.add_argument('--librarian', default='llvm-lib')
    parser.add_argument('--msvc-sysroot', type=Path)
    with tempfile.TemporaryDirectory() as directory:
        build(parser.parse_args(), Path(directory))
