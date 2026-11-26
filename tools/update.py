#!/usr/bin/env python3
"""Brings this copy of the game up to date in one go (docs/setup/update.md): pulls the newest main from GitHub, builds
the editor at low priority, then runs only the one-off art steps (tools/art_steps.json) whose inputs changed since they
last ran here. tools/update-and-play.bat runs this and then starts the game on the title screen.

  python tools/update.py               pull, build, art steps
  python tools/update.py --list        show every art step and whether it would run, change nothing
  python tools/update.py --adopt       mark every art step as done for the files as they are now, run nothing
                                       (after the steps were run by hand)
  python tools/update.py --force hair-grow blood-art   run these steps even if nothing changed
  python tools/update.py --no-pull --no-build --offline   skip parts (offline: skip steps that need internet)

Exit code: 0 all done, 1 an art step failed (the game still runs, without that step's update), 2 the pull or the build
failed or the game is open.
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STEPS_FILE = ROOT / "tools" / "art_steps.json"
PROJECT = ROOT / "game" / "Rim3D.uproject"
STATE_DIR = ROOT / "game" / "Saved" / "ArtSteps"
STAMPS_FILE = STATE_DIR / "stamps.json"
SOURCE_DIRS = [ROOT / "game" / "Source" / "Rim3DEditor", ROOT / "game" / "Source" / "Rim3D"]
DEFAULT_ENGINE = r"C:\Program Files\Epic Games\UE_5.8"
DEFAULT_BLENDER = r"C:\Program Files\Blender Foundation\Blender 5.2\blender.exe"
# Folders outside the repo that steps name as {key} in their commands and raw folders.
PLACES = {
    "codex": os.environ.get("RIM3D_CODEX", os.path.expanduser(os.path.join("~", "OneDrive", "Documents", "ChatGPT", "Models", "Rim3D_Codex"))),
}
BELOW_NORMAL = 0x00004000  # Windows BELOW_NORMAL_PRIORITY_CLASS: sneed's games and tools keep the CPU


def say(*parts):
    print("[update]", *parts, flush=True)


# ---- The steps and their fingerprints ---------------------------------------------------------------------------

def load_steps(path=STEPS_FILE):
    steps = json.loads(Path(path).read_text(encoding="utf-8"))["steps"]
    problems = check_steps(steps)
    if problems:
        raise SystemExit("tools/art_steps.json: " + "; ".join(problems))
    return steps


def check_steps(steps):
    """Names are unique, and 'after' and 'coveredBy' name steps earlier in the list (they run in order)."""
    problems = []
    seen = set()
    for step in steps:
        name = step.get("name")
        if not name or name in seen:
            problems.append(f"step name {name!r} missing or used twice")
        for key in ("after", "coveredBy"):
            for other in step.get(key, []):
                if other not in seen:
                    problems.append(f"{name}: {key} {other!r} is not an earlier step")
        if not step.get("run"):
            problems.append(f"{name}: nothing to run")
        for command in step.get("run", []):
            if len([k for k in ("python", "blender", "commandlet") if k in command]) != 1:
                problems.append(f"{name}: each run entry needs exactly one of python, blender, commandlet")
        seen.add(name)
    return problems


def git_blobs(patterns, root=ROOT):
    """{path: blob id} for the files in Git matching the globs (Git's own ids: no file is read)."""
    if not patterns:
        return {}
    out = subprocess.run(["git", "ls-files", "-s", "--"] + [f":(glob){p}" for p in patterns], cwd=root,
                         capture_output=True, text=True, check=False).stdout
    blobs = {}
    for line in out.splitlines():
        meta, _, path = line.partition("\t")
        parts = meta.split()
        if len(parts) >= 2:
            blobs[path] = parts[1]
    return blobs


INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"/]+\.h)"', re.M)


def commandlet_sources(name, source_dirs=SOURCE_DIRS, root=ROOT):
    """The commandlet's own .cpp/.h and the project headers it includes (followed inside the editor module; a header
    of the game module is counted but not followed, so a commandlet does not depend on half the game)."""
    editor = source_dirs[0]
    found = []
    todo = [editor / f"{name}Commandlet.cpp", editor / f"{name}Commandlet.h"]
    while todo:
        path = todo.pop()
        if path in found or not path.exists():
            continue
        found.append(path)
        if path.parent != editor:
            continue
        for header in INCLUDE.findall(path.read_text(encoding="utf-8", errors="replace")):
            for folder in source_dirs:
                if (folder / header).exists():
                    todo.append(folder / header)
                    break
    return sorted(p.relative_to(root).as_posix() for p in found)


def expand(text, places=None):
    """{codex} and the like -> the folder (PLACES)."""
    for key, value in (PLACES if places is None else places).items():
        text = text.replace("{" + key + "}", value)
    return text


def raw_listing(folders, root=ROOT):
    """Names, sizes and modified times of every file under the folders (downloads and Blender output, not in Git)."""
    rows = []
    for folder in folders:
        base = root / expand(folder)
        if not base.exists():
            rows.append(f"{folder}: missing")
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames.sort()
            for f in sorted(filenames):
                p = Path(dirpath) / f
                try:
                    st = p.stat()
                except OSError:
                    continue
                try:
                    name = p.relative_to(root).as_posix()
                except ValueError:  # a folder outside the repo ({codex})
                    name = p.relative_to(base).as_posix()
                rows.append(f"{folder}/{name} {st.st_size} {int(st.st_mtime)}")
    return rows


def fingerprint(step, root=ROOT, source_dirs=SOURCE_DIRS):
    patterns = list(step.get("inputs", []))
    for command in step.get("run", []):
        if "commandlet" in command:
            patterns += commandlet_sources(command["commandlet"], source_dirs, root)
    if step.get("commandletSource"):
        patterns += commandlet_sources(step["commandletSource"], source_dirs, root)
    h = hashlib.sha256()
    h.update(json.dumps(step.get("run"), sort_keys=True).encode())
    for path, blob in sorted(git_blobs(sorted(set(patterns)), root).items()):
        h.update(f"{path} {blob}\n".encode())
    for row in raw_listing(step.get("raw", []), root):
        h.update((row + "\n").encode())
    return h.hexdigest()


def load_stamps():
    try:
        return json.loads(STAMPS_FILE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def save_stamps(stamps):
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    STAMPS_FILE.write_text(json.dumps(stamps, indent=2, sort_keys=True), encoding="utf-8")


def plan(steps, stamps, prints, forced=(), have=lambda need: True, root=ROOT):
    """Decides each step in order. prints: {name: fingerprint}. Returns [(step, decision, why)] where decision is
    'run', 'skip' (up to date), 'covered' (an earlier step this update does its work), 'wait' (a need is missing) or
    'failed' (it failed last time on these same inputs). 'run' assumes every step before it succeeds; update() re-plans
    the rest after a failure."""
    running = set()
    out = []
    for step in steps:
        name = step["name"]
        stamp = stamps.get(name, {})
        why = None
        if name in forced:
            why = "asked for"
        elif stamp.get("done") != prints[name]:
            why = "first run here" if not stamp.get("done") else "its inputs changed"
        triggers = [a for a in step.get("after", []) if a in running]
        if not why and triggers:
            why = "after " + ", ".join(triggers)
        if not why:
            missing = [m for m in step.get("makes", []) if not (root / m).exists()]
            if missing:
                why = "missing " + missing[0]
        if not why:
            out.append((step, "skip", "up to date"))
            continue
        covered = [c for c in step.get("coveredBy", []) if c in running]
        if covered:
            out.append((step, "covered", "done by " + covered[0]))
            continue
        lacking = [n for n in step.get("needs", []) if not have(n)]
        if lacking:
            out.append((step, "wait", "needs " + ", ".join(lacking)))
            continue
        if name not in forced and stamp.get("failed") == prints[name] and not triggers:
            out.append((step, "failed", f"failed last time on these inputs (see its log; --force {name} to retry)"))
            continue
        running.add(name)
        out.append((step, "run", why))
    return out


# ---- Running things -----------------------------------------------------------------------------------------------

def run_logged(cmd, log_path, cwd=ROOT):
    log_path.parent.mkdir(parents=True, exist_ok=True)
    flags = BELOW_NORMAL if os.name == "nt" else 0
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        log.write(" ".join(str(c) for c in cmd) + "\n\n")
        log.flush()
        proc = subprocess.run([str(c) for c in cmd], cwd=cwd, stdout=log, stderr=subprocess.STDOUT, creationflags=flags)
    return proc.returncode


def command_line(command, engine, blender):
    if "python" in command:
        return [sys.executable] + [expand(a) for a in command["python"]]
    if "blender" in command:
        return [blender] + [expand(a) for a in command["blender"]]
    editor_cmd = Path(engine) / "Engine" / "Binaries" / "Win64" / "UnrealEditor-Cmd.exe"
    return [editor_cmd, PROJECT, f"-run={command['commandlet']}", "-unattended", "-nosplash"] + command.get("args", [])


def game_running():
    if os.name != "nt":
        return False
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq UnrealEditor.exe", "/NH"], capture_output=True, text=True).stdout
    out += subprocess.run(["tasklist", "/FI", "IMAGENAME eq UnrealEditor-Cmd.exe", "/NH"], capture_output=True, text=True).stdout
    return "UnrealEditor" in out


def git(*args, check=True):
    say("git", " ".join(args))
    return subprocess.run(["git"] + list(args), cwd=ROOT, check=check).returncode


def pull():
    branch = subprocess.run(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    if branch != "main":
        say(f"on branch {branch}, not main: not pulling (this is a test copy)")
        return True
    # The editor writes a phone file-server token into DefaultEngine.ini when it runs; it is never wanted in Git.
    subprocess.run(["git", "checkout", "--", "game/Config/DefaultEngine.ini"], cwd=ROOT, check=False)
    before = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    if git("pull", "--ff-only", "--autostash", "origin", "main", check=False) != 0:
        say("could not pull the newest version (see above); carrying on with the version here")
        return False
    after = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    if before != after:
        subprocess.run(["git", "--no-pager", "log", "--oneline", f"{before}..{after}"], cwd=ROOT, check=False)
    else:
        say("already the newest version")
    return True


def build(engine, threads):
    build_bat = Path(engine) / "Engine" / "Build" / "BatchFiles" / "Build.bat"
    say(f"building the game (low priority, {threads} jobs); log in game/Saved/ArtSteps/logs/build.log")
    started = time.time()
    code = run_logged([build_bat, "Rim3DEditor", "Win64", "Development", f"-Project={PROJECT}", f"-MaxParallelActions={threads}", "-WaitMutex"],
                      STATE_DIR / "logs" / "build.log")
    say(f"build {'ok' if code == 0 else 'FAILED'} in {time.time() - started:.0f} s")
    return code == 0


def update(args):
    steps = load_steps()
    stamps = load_stamps()
    blender = os.environ.get("BLENDER", DEFAULT_BLENDER)
    engine = os.environ.get("UE_ROOT", args.engine)
    have = {"internet": not args.offline, "blender": Path(blender).exists(), "codex": Path(PLACES["codex"]).exists()}
    prints = {s["name"]: fingerprint(s) for s in steps}
    unknown = [f for f in args.force if f not in prints]
    if unknown:
        raise SystemExit("no such step: " + ", ".join(unknown) + " (python tools/update.py --list)")

    if args.adopt:
        for s in steps:
            stamps[s["name"]] = {"done": prints[s["name"]], "when": time.strftime("%Y-%m-%d %H:%M")}
        save_stamps(stamps)
        say(f"marked all {len(steps)} art steps as done for the files as they are now")
        return 0

    decisions = plan(steps, stamps, prints, set(args.force), lambda n: have.get(n, True))
    if args.list:
        for step, decision, why in decisions:
            print(f"  {decision:8} {step['name']:20} {why}  ({step.get('what', '')})")
        return 0

    failed = []
    ran = set()
    try:
        return run_steps(steps, stamps, prints, decisions, engine, blender, failed, ran)
    except KeyboardInterrupt:
        # Ctrl+C or a closed window: whatever was running is left half done, so it is not stamped (done or failed)
        # and runs again on the next update.
        save_stamps(stamps)
        say("stopped part way through; the unfinished art step runs again next time (run update-and-play again)")
        return 2


INTERRUPTED_CODES = {130, -2, 0xC000013A, -1073741510}  # Ctrl+C / console closed (POSIX, Windows STATUS_CONTROL_C_EXIT)


def stamp_done(stamps, step):
    """Done, with the step's fingerprint taken now: its raw folders may hold what earlier steps in this update just
    wrote (Blender bodies, grown hair), and a fingerprint from before them would make it run again next time."""
    stamps[step["name"]] = {"done": fingerprint(step), "when": time.strftime("%Y-%m-%d %H:%M")}


def run_steps(steps, stamps, prints, decisions, engine, blender, failed, ran):
    for step, decision, why in decisions:
        name = step["name"]
        # A step that comes after one that failed waits for next time, unless it has reasons of its own.
        if decision == "run" and why.startswith("after ") and not any(a in ran for a in step.get("after", [])):
            say(f"- {name}: waits (the step before it failed)")
            continue
        if decision == "covered" and not any(c in ran for c in step.get("coveredBy", [])):
            decision, why = "run", why.replace("done by", "instead of")
        if decision != "run":
            if decision in ("wait", "failed"):
                say(f"- {name}: {why}")
            continue
        say(f"+ {name}: {step.get('what', '')} ({why})")
        started = time.time()
        ok = True
        for i, command in enumerate(step["run"]):
            log = STATE_DIR / "logs" / f"{name}{'' if i == 0 else '-' + str(i + 1)}.log"
            code = run_logged(command_line(command, engine, blender), log)
            if code in INTERRUPTED_CODES:
                raise KeyboardInterrupt
            if code != 0:
                say(f"  FAILED, log: {log}")
                ok = False
                break
        say(f"  {'done' if ok else 'failed'} in {time.time() - started:.0f} s")
        if ok:
            ran.add(name)
            stamp_done(stamps, step)
            for s in steps:  # steps this one covers count as done too
                if name in s.get("coveredBy", []) and any(d[0] is s and d[1] == "covered" for d in decisions):
                    stamp_done(stamps, s)
        else:
            failed.append(name)
            stamps.setdefault(name, {})["failed"] = fingerprint(step)
        save_stamps(stamps)
    if failed:
        say("art steps that failed: " + ", ".join(failed) + " (the game still runs; logs in game/Saved/ArtSteps/logs)")
        return 1
    say("art steps up to date")
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--list", action="store_true", help="show what would run, change nothing")
    p.add_argument("--adopt", action="store_true", help="mark every art step as done for the files as they are now")
    p.add_argument("--force", nargs="*", default=[], help="art steps to run even if nothing changed")
    p.add_argument("--no-pull", action="store_true")
    p.add_argument("--no-build", action="store_true")
    p.add_argument("--no-art", action="store_true")
    p.add_argument("--offline", action="store_true", help="skip the art steps that need internet")
    p.add_argument("--engine", default=DEFAULT_ENGINE)
    p.add_argument("--threads", type=int, default=6)
    args = p.parse_args(argv)

    if args.list or args.adopt:
        return update(args)
    if game_running():
        say("Unreal or the game is open: close it first, then run this again")
        return 2
    if not args.no_pull:
        pull()
    if not args.no_build and not build(args.engine, args.threads):
        say("the build failed; the last lines of game/Saved/ArtSteps/logs/build.log:")
        lines = (STATE_DIR / "logs" / "build.log").read_text(encoding="utf-8", errors="replace").splitlines()
        print("\n".join(lines[-25:]))
        return 2
    if args.no_art:
        return 0
    return update(args)


if __name__ == "__main__":
    sys.exit(main())
