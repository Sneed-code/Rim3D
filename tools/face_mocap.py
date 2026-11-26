"""Packs facial motion capture (Apple ARKit's 52 blendshape curves) into assets/face_mocap/clips.json for the game's live
faces (docs/design/faces-mocap.md, Unreal PawnFaceMocap.cpp).

    python tools/face_mocap.py <folder of capture .json files> <labels.json> [--out assets/face_mocap/clips.json]

The captures are the BEAT dataset's facial files (one take each: {"names": [52 curve names], "frames": [{"time": s,
"weights": [52]}]}, 60 fps). labels.json maps each file's name (without .json) to its emotion (neutral, happiness, anger,
sadness, contempt, surprise, fear, disgust). Each take is resampled to 30 fps, lightly smoothed (the capture jitters),
cut into clips of 6 to 14 seconds at quiet moments (the eyes open, the mouth nearly shut) so a clip ends where another
can start, and each curve stored as a byte (0..255) per frame, base64 in the file. The curves are put in ARKit's own order
whatever order the file had.
"""
import argparse
import base64
import json
import os
import sys

ARKIT = [
    "eyeBlinkLeft", "eyeLookDownLeft", "eyeLookInLeft", "eyeLookOutLeft", "eyeLookUpLeft", "eyeSquintLeft", "eyeWideLeft",
    "eyeBlinkRight", "eyeLookDownRight", "eyeLookInRight", "eyeLookOutRight", "eyeLookUpRight", "eyeSquintRight", "eyeWideRight",
    "jawForward", "jawLeft", "jawRight", "jawOpen", "mouthClose", "mouthFunnel", "mouthPucker", "mouthLeft", "mouthRight",
    "mouthSmileLeft", "mouthSmileRight", "mouthFrownLeft", "mouthFrownRight", "mouthDimpleLeft", "mouthDimpleRight",
    "mouthStretchLeft", "mouthStretchRight", "mouthRollLower", "mouthRollUpper", "mouthShrugLower", "mouthShrugUpper",
    "mouthPressLeft", "mouthPressRight", "mouthLowerDownLeft", "mouthLowerDownRight", "mouthUpperUpLeft", "mouthUpperUpRight",
    "browDownLeft", "browDownRight", "browInnerUp", "browOuterUpLeft", "browOuterUpRight", "cheekPuff", "cheekSquintLeft",
    "cheekSquintRight", "noseSneerLeft", "noseSneerRight", "tongueOut"]
EMOTIONS = ["neutral", "happiness", "anger", "sadness", "contempt", "surprise", "fear", "disgust"]
FPS = 30.0


def canonical(name):
    """A curve name in ARKit's spelling: BEAT and Live Link spell them alike, some tools add a prefix or capitals."""
    n = name.split(".")[-1].split("_")[-1]
    for a in ARKIT:
        if a.lower() == n.lower():
            return a
    return None


def read_take(path):
    """(times, frames): frames as lists of 52 floats in ARKit order."""
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    names = data.get("names") or data.get("blendshape_names") or ARKIT
    order = [canonical(n) for n in names]
    times, frames = [], []
    for i, frame in enumerate(data["frames"]):
        weights = frame["weights"] if isinstance(frame, dict) else frame
        values = [0.0] * len(ARKIT)
        for name, w in zip(order, weights):
            if name:
                values[ARKIT.index(name)] = float(w)
        times.append(float(frame.get("time", i / 60.0)) if isinstance(frame, dict) else i / 60.0)
        frames.append(values)
    return times, frames


def resample(times, frames, fps=FPS):
    if len(frames) < 2:
        return []
    out = []
    j = 0
    t = times[0]
    while t <= times[-1]:
        while j + 1 < len(times) - 1 and times[j + 1] < t:
            j += 1
        t0, t1 = times[j], times[j + 1]
        a = 0.0 if t1 <= t0 else min(max((t - t0) / (t1 - t0), 0.0), 1.0)
        out.append([x + (y - x) * a for x, y in zip(frames[j], frames[j + 1])])
        t += 1.0 / fps
    return out


def smooth(frames):
    """A 3-frame weighted average per curve, blinks kept (a blink lasts about 4 frames at 30 fps)."""
    if len(frames) < 3:
        return frames
    out = [frames[0]]
    for i in range(1, len(frames) - 1):
        out.append([0.25 * a + 0.5 * b + 0.25 * c for a, b, c in zip(frames[i - 1], frames[i], frames[i + 1])])
    out.append(frames[-1])
    return out


def quiet_score(frame):
    """How good a frame is to cut at: eyes open, jaw nearly shut, little going on."""
    blink = max(frame[ARKIT.index("eyeBlinkLeft")], frame[ARKIT.index("eyeBlinkRight")])
    jaw = frame[ARKIT.index("jawOpen")]
    return blink + 2.0 * jaw


def cut(frames, shortest=6.0, longest=14.0, fps=FPS):
    """Clips of shortest..longest seconds, each cut at the quietest frame in its last third."""
    clips = []
    start = 0
    lo, hi = int(shortest * fps), int(longest * fps)
    while len(frames) - start >= lo:
        end = min(start + hi, len(frames))
        if end < len(frames):
            window = range(start + lo, end)
            end = min(window, key=lambda i: quiet_score(frames[i])) + 1
        clips.append(frames[start:end])
        start = end
    return clips


def pack(frames):
    raw = bytearray()
    for frame in frames:
        raw.extend(max(0, min(255, int(round(v * 255.0)))) for v in frame)
    return base64.b64encode(bytes(raw)).decode("ascii")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("folder")
    parser.add_argument("labels")
    parser.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "face_mocap", "clips.json"))
    parser.add_argument("--per-emotion-seconds", type=float, default=150.0, help="at most this much of each emotion")
    parser.add_argument("--per-take-seconds", type=float, default=40.0, help="at most this much of one take (more people and takes)")
    args = parser.parse_args(argv)
    with open(args.labels, encoding="utf-8") as f:
        labels = json.load(f)
    seconds = {e: 0.0 for e in EMOTIONS}
    clips = []
    # Round robin over the speakers (BEAT names start <id>_<speaker>_), so every emotion has several people.
    def speaker(n):
        parts = n.split("_")
        return parts[1] if len(parts) > 1 else n
    by_speaker = {}
    for n in sorted(labels):
        by_speaker.setdefault(speaker(n), []).append(n)
    order = []
    while any(by_speaker.values()):
        for names in by_speaker.values():
            if names:
                order.append(names.pop(0))
    for name in order:
        emotion = labels[name]
        if emotion not in EMOTIONS:
            print("skipped", name, "(emotion", emotion + ")")
            continue
        path = os.path.join(args.folder, name + ".json")
        if not os.path.exists(path):
            print("missing", path)
            continue
        if seconds[emotion] >= args.per_emotion_seconds:
            continue
        times, frames = read_take(path)
        frames = smooth(resample(times, frames))[: int(args.per_take_seconds * FPS)]
        for k, clip in enumerate(cut(frames)):
            if seconds[emotion] >= args.per_emotion_seconds:
                break
            clips.append({"name": f"{name}_{k}", "emotion": emotion, "frames": len(clip), "data": pack(clip)})
            seconds[emotion] += len(clip) / FPS
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump({"comment": "Facial capture clips for the live faces (tools/face_mocap.py, docs/design/faces-mocap.md). Source: the BEAT dataset (Liu et al. 2022, CC BY-NC 4.0, non-commercial).",
                   "fps": FPS, "channels": ARKIT, "clips": clips}, f, separators=(",", ":"))
    print(len(clips), "clips;", ", ".join(f"{e} {s:.0f} s" for e, s in seconds.items()), "->", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
