#!/usr/bin/env python3
"""Play a Ranked set between the Mac fork and Slippi Dolphin 3.7.0 on loopback."""
import argparse
import asyncio
from collections import Counter
from contextlib import ExitStack
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time

import melee
import ubjson
from websockets.asyncio.server import serve

from check_local_ranked_set import controller, two_ports


def local_user(seat):
    return {"uid": f"fixture{seat}", "playKey": "local-test-only", "displayName": ("Native", "Dolphin")[seat],
            "connectCode": f"TEST#{20 + seat}", "latestVersion": "3.7.0"}


def prepare_stock(app, work, spectator_port, peer_port):
    user = work / "User"
    config = user / "Config"
    config.mkdir(parents=True)
    account = work / "home/Library/Application Support/com.project-slippi.dolphin/Slippi"
    account.mkdir(parents=True)
    (account / "user.json").write_text(json.dumps(local_user(1)))
    (config / "Dolphin.ini").write_text(f"""[Core]
EnableCheats = True
DSPHLE = True
SIDevice0 = 6
SIDevice1 = 0
SIDevice2 = 0
SIDevice3 = 0
SlippiEnableSpectator = True
SlippiSpectatorLocalPort = {spectator_port}
SlippiForceNetplayPort = True
SlippiNetplayPort = {peer_port}
SlippiForceLanIp = True
SlippiLanIp = 127.0.0.1
SlippiSaveReplays = True
SlippiReplayMonthFolders = False
SlippiReplayDir = {work / 'games'}
SlippiJukeboxEnabled = False
BlockingPipes = False
[Interface]
PanicHandlers = False
ConfirmStop = False
PauseOnFocusLost = False
[Display]
Fullscreen = False
RenderToMain = True
[Input]
BackgroundInput = True
[DSP]
Backend = No audio output
Volume = 0
[Analytics]
Enabled = False
PermissionAsked = True
""")
    (config / "Logger.ini").write_text("[Options]\nWriteToFile=True\nVerbosity=4\n[Logs]\n"
                                       "SLIPPI=True\nSLIPPI_ONLINE=True\nSLIPPI_RUST_ONLINE=True\n")
    pad = (app / "Contents/Resources/Config/Profiles/GCPad/slippibot.ini").read_text()
    (config / "GCPadNew.ini").write_text(pad.replace("[Profile]", "[GCPad1]"))
    # This adds libmelee's documented menu telemetry for the test controller.
    # The stock app, disc, online game codes and gameplay rules stay intact.
    name = "$Optional: Extract Menu Info"
    codes = (Path(melee.__file__).parent / "GALE01r2.ini").read_text()
    definition = next(block for block in codes.split("\n$") if block.startswith(name[1:] + " ["))
    (user / "GameSettings").mkdir()
    (user / "GameSettings/GALE01r2.ini").write_text(f"[Gecko_Enabled]\n{name}\n[Gecko]\n${definition}\n")
    pipes = user / "Pipes"
    pipes.mkdir()
    path = pipes / "slippibot1"
    os.mkfifo(path)
    return path


def start_game(stack, command, work, url, extra=None):
    # Some Apple APIs still use the account home from the OS even with HOME set.
    # Deny both real credential directories and permit only loopback outbound traffic.
    protected = [Path.home() / "Library/Application Support" / name
                 for name in ("Slippi Launcher", "com.project-slippi.dolphin")]
    filters = " ".join(f"(subpath {json.dumps(str(path))})" for path in protected)
    sandbox = ('(version 1)(allow default)(deny network-outbound)'
               '(allow network-outbound (remote ip "localhost:*"))'
               f'(deny file-read* file-write* {filters})')
    (work / "home").mkdir(exist_ok=True)
    log = stack.enter_context((work / "console.log").open("wb"))
    env = {"PATH": os.environ["PATH"], "HOME": str(work / "home"), "NO_PROXY": "*", "SLIPPI_MM_URL": url,
           **(extra or {})}
    proc = subprocess.Popen(["/usr/bin/sandbox-exec", "-p", sandbox, *command], cwd=work,
                            stdout=log, stderr=subprocess.STDOUT, env=env)

    def stop():
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

    stack.callback(stop)
    return proc


class Queue:
    def __init__(self):
        self.joins = {}
        self.ready = asyncio.Event()

    async def handle(self, ws):
        join = json.loads(await ws.recv())
        seat = next(i for i in range(2) if join["user"]["uid"] == local_user(i)["uid"])
        if (ws.request.path, join["type"], join["appVersion"], join["search"]["mode"]) != ("/v1/queue", "join", "3.7.0", 0):
            raise ValueError("Expected a 3.7.0 Ranked join")
        self.joins[seat] = join
        await ws.send(json.dumps({"type": "joined", "token": "local", "ticketId": str(seat)}))
        if len(self.joins) == 2:
            self.ready.set()
        while not self.ready.is_set():
            await ws.send(json.dumps({"type": "status", "queuePosition": 1}))
            try:
                await asyncio.wait_for(self.ready.wait(), 2)
            except asyncio.TimeoutError:
                continue
        players = []
        for i in range(2):
            user = local_user(i)
            address = f"127.0.0.1:{int(self.joins[i]['port'])}"
            players.append({key: user[key] for key in ("uid", "displayName", "connectCode")} |
                           {"port": i + 1, "isLocalPlayer": i == seat, "ipAddress": address, "ipAddressLan": address})
        await ws.send(json.dumps({"type": "match", "matchId": "mode.ranked-cross-test", "isHost": seat == 0,
                                  "players": players, "stages": [2, 3, 8, 28, 31, 32]}))
        await ws.wait_closed()


def drive_stock(pipe, state, tick):
    if tick % 9 == 4:
        os.write(pipe, b"RELEASE A\nRELEASE START\nRELEASE D_RIGHT\n")
    if tick % 9 or state is None:
        return
    commands = ["RELEASE A", "RELEASE START", "RELEASE D_RIGHT", "SET MAIN 0.5 0.5"]
    scene = state.menu_state
    if scene == melee.Menu.MAIN_MENU:
        row = state.submenu == melee.SubMenu.ONLINE_PLAY_SUBMENU and state.menu_selection != 0
        commands.append("SET MAIN 0.5 0" if row else "PRESS A")
    elif scene == melee.Menu.PRESS_START:
        commands.append("PRESS START")
    elif scene == melee.Menu.SLIPPI_ONLINE_CSS and 1 in state.players:
        player = state.players[1]
        if player.coin_down or (player.character != melee.Character.UNKNOWN_CHARACTER and tick % 27 == 0):
            commands.append("PRESS START")
        elif player.cursor.y < 15:
            commands.append("SET MAIN 0.5 1")
        elif player.cursor.x < -10:
            commands.append("SET MAIN 1 0.5")
        else:
            commands.append("PRESS A")
    elif scene == melee.Menu.IN_GAME:
        commands.append("SET MAIN 1 0.5")  # Walk the stock-side fighter off each respawn.
    else:
        commands.append("PRESS A" if tick % 27 else "PRESS D_RIGHT")
    os.write(pipe, ("\n".join(commands) + "\n").encode())


def fighter_records(path):
    raw = ubjson.loadb(path.read_bytes())["raw"]
    if raw[0] != 0x35:
        raise ValueError(f"Missing event size table in {path}")
    end = raw[1] + 1
    sizes = {raw[j]: int.from_bytes(raw[j + 1:j + 3], "big") + 1 for j in range(2, end, 3)}
    offset = end
    records, ending = {}, None
    while offset < len(raw):
        event = raw[offset]
        body = raw[offset:offset + sizes[event]]
        if event == 0x38:
            key = (int.from_bytes(body[1:5], "big", signed=True), body[5], body[6])
            records[key] = body  # Keep the final simulation of this frame after any rollback.
        elif event == 0x39:
            ending = body
        offset += len(body)
    return records, ending


def compare_game(native_path, stock_path):
    native, end_native = fighter_records(native_path)
    stock, end_stock = fighter_records(stock_path)
    if not native or native.keys() != stock.keys() or end_native != end_stock or end_native is None:
        raise ValueError(f"Frame coverage or game end differs: {native_path.name}, {stock_path.name}")
    fields = Counter()
    identical = 0
    for key, a in native.items():
        b = stock[key]
        if len(a) != len(b):
            raise ValueError(f"Post-frame length differs at {key}")
        identical += a == b
        for pos, (left, right) in enumerate(zip(a, b)):
            if left != right:
                fields[(pos, left ^ right)] += 1
    print(f"{native_path.name}: {identical}/{len(native)} byte-identical fighter records; "
          f"differing (byte, mask) counts {dict(fields)}", flush=True)
    return fields


async def run(args, work):
    native = work / "fork"
    (native / "user").mkdir(parents=True)
    (native / "user/user.json").write_text(json.dumps(local_user(0)))
    (native / "pad.txt").write_text(controller(0))
    stock = work / "stock"
    peer_port, spectator_port = two_ports()
    fifo = prepare_stock(args.dolphin, stock, spectator_port, peer_port)
    queue = Queue()
    async with serve(queue.handle, "127.0.0.1", 0) as server:
        url = f"ws://127.0.0.1:{server.sockets[0].getsockname()[1]}/v1/queue"
        with ExitStack() as stack:
            pipe = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
            stack.callback(os.close, pipe)
            old = start_game(stack, [args.dolphin / "Contents/MacOS/Slippi Dolphin", "-u", stock / "User",
                                     "-b", "-c", "False", "-e", args.iso, "-v", "OGL", "--slippi-spectator-port",
                                     str(spectator_port), "-a", "HLE"], stock, url)
            new = start_game(stack, [args.fork / "Contents/MacOS/MeleeUnlocked", "--iso", args.iso, "--headless",
                                     "--allow-matchmaking", "--volume", "0", "--slippi-menus", "on",
                                     "--user-dir", native / "user", "--sys-dir", args.fork / "Contents/Resources/Sys",
                                     "--card-dir", native / "cards", "--replay-dir", native / "games",
                                     "--settings-path", native / "settings.ini", "--script", native / "pad.txt",
                                     "--frames", "24000", "--log-file", native / "host.log"], native, url,
                             {"MELEE_NO_GC_ADAPTER": "1", "SDL_AUDIODRIVER": "dummy",
                              "MELEE_TEST_SCRIPT_PAD_FRAMES": "1", "ROSETTA_ADVERTISE_AVX": "1"})
            await asyncio.sleep(7)
            spectator = melee.Console(is_remote=True, slippi_port=spectator_port, polling_mode=True, setup_gecko_codes=False)
            if not await asyncio.to_thread(spectator.connect):
                raise RuntimeError("Could not connect to the stock spectator port")
            stack.callback(spectator.stop)
            state, tick = None, 0
            deadline = time.monotonic() + 420
            while time.monotonic() < deadline:
                if old.poll() is not None or new.poll() is not None:
                    raise RuntimeError("A game exited early")
                fresh = spectator.step()
                if fresh is not None:
                    state = fresh
                tick += 1
                drive_stock(pipe, state, tick)
                if tick % 45 == 0:
                    host = (native / "host.log").read_text(errors="replace")
                    if "ranked set: over" in host:
                        break
                    if "ranked set: abandoned" in host:
                        raise RuntimeError("Ranked set was abandoned")
                await asyncio.sleep(.015)
            else:
                raise RuntimeError("Timed out waiting for the Ranked set")
            await asyncio.sleep(2)  # Allow the stock recorder to finalize its file.
            stock_log = (stock / "User/Logs/dolphin.log").read_text(errors="replace")
            fork_reports = re.findall(r"game report: mode 0, (\d+) frames, game (\d+), tiebreak 0, winner (\d+), end 2, lras -1, stage (\d+)", host)
            stock_reports = re.findall(r"Frames: (\d+), GameIdx: (\d+), TiebreakIdx: 0, WinnerIdx: (\d+), StageId: (\d+), GameEndMethod: 2, LRASInitiator: -1", stock_log)
            if len(fork_reports) < 2 or fork_reports != stock_reports or len(queue.joins) != 2:
                raise ValueError(f"Conflicting game reports: fork={fork_reports}, stock={stock_reports}")
            print(f"Both clients completed the Ranked set: {fork_reports}", flush=True)
            native_games = sorted((native / "games").glob("*.slp"))
            stock_games = sorted((stock / "games").glob("*.slp"))
            if len(native_games) != len(stock_games) or len(native_games) != len(fork_reports):
                raise ValueError("A game recording is missing")
            for a, b in zip(native_games, stock_games):
                compare_game(a, b)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dolphin", required=True, type=Path, help="Installed/copied Slippi 3.7.0 netplay .app, outside its DMG")
    parser.add_argument("--fork", required=True, type=Path, help="Packaged MeleeUnlocked.app")
    parser.add_argument("--iso", required=True, type=Path)
    parser.add_argument("--output", type=Path, help="New directory for isolated profiles, logs and recordings")
    args = parser.parse_args()
    if sys.platform != "darwin" or not Path("/usr/bin/sandbox-exec").is_file():
        parser.error("This harness requires the macOS network sandbox")
    args.dolphin, args.fork, args.iso = (path.resolve(strict=True) for path in (args.dolphin, args.fork, args.iso))
    work = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="stock-ranked-"))
    if args.output:
        work.mkdir(parents=True, exist_ok=False)
    print(f"Profiles and results in {work}", flush=True)
    asyncio.run(run(args, work))


if __name__ == "__main__":
    main()
