"""Cut the AI Studio sounds (docs/design/neon-assets.md, "Sounds in the game") into the game's own cue folders.

Run from the repo root:
  python tools/neon_sounds.py <sounds folder>
The folder is the split set the Neon thread made (male_gruff_set1/2, female_pain, female_begging, female_pleasure as single
WAV takes, and the two Lyria Clip MP3s). The output is committed (game/Content/Rim3D/Sounds/cues/<Cue>/neon_*.wav), so the
PC does not need the source files; run this again only to change the picks below. Needs ffmpeg and numpy.

Every clip: mono, 44.1 kHz, 16-bit, starting on the sound (silence under -45 dB trimmed, 5 ms fade in, 60 ms fade out),
peak at -1 dB like sneed's own voice files. Takes that are cut off or are only a breath are left out: quieter than
-20 dB at their peak, or under 0.2 s of sound.
"""
import os
import random
import subprocess
import sys
import wave

import numpy as np

SR = 44100
OUT = os.path.join("game", "Content", "Rim3D", "Sounds", "cues")

# Left out by ear-free checks (see the docstring) and the begging blip between two phrases.
SKIP = {"female_begging_02", "female_begging_09", "female_pain_14", "female_pleasure_01", "female_pleasure_04",
        "female_pleasure_14", "female_pleasure_16", "female_pleasure_17"}

# Short grunts for a hit, longer groans for lying downed: split by length (seconds of sound).
HIT_MAX = 0.75

# Windows cut from the 30 s Lyria Clip sound design (start, seconds). The robot clip steps its servos about once a second
# and has two places where it stops dead (16 s, 24 s); the insect clip chitters all the way with a hiss every 2 s and
# stops at 20 s.
LYRIA = {
    "rim3d_lyria_test_mechanoid.mp3": {
        "Animal_Mech_Idle": [(1.75, 2.0), (3.75, 2.0), (5.75, 2.0), (9.75, 2.0), (11.75, 2.0), (13.75, 2.0), (17.0, 2.2), (25.0, 2.2)],
        "Animal_Mech_Hurt": [(0.1, 0.9), (7.55, 0.8), (8.3, 0.7), (4.45, 0.6)],
        "Animal_Mech_Death": [(14.6, 1.5), (22.4, 1.6)],
    },
    "rim3d_lyria_test_insectoid.mp3": {
        "Animal_Chitinid_Idle": [(1.0, 2.0), (5.0, 2.0), (9.0, 2.0), (13.0, 2.0), (21.0, 2.0), (26.0, 2.0)],
        "Animal_Chitinid_Hurt": [(3.35, 0.7), (15.35, 0.7), (17.85, 0.7), (23.35, 0.7), (29.35, 0.7)],
        "Animal_Chitinid_Death": [(18.4, 1.65)],
    },
}


def load(path):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", str(SR), "-f", "f32le", "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.float32).astype(np.float64)


def trim(a, floor_db=-45.0):
    """Cut silence off both ends: 10 ms windows quieter than floor_db (dBFS)."""
    w = int(0.01 * SR)
    n = len(a) // w
    if n == 0:
        return a
    env = 20 * np.log10(np.sqrt((a[:n * w].reshape(n, w) ** 2).mean(1)) + 1e-12)
    loud = np.nonzero(env > floor_db)[0]
    if len(loud) == 0:
        return a[:0]
    start = max(loud[0] - 1, 0) * w
    end = min((loud[-1] + 3) * w, len(a))
    return a[start:end]


def finish(a, fade_in=0.005, fade_out=0.06):
    a = a.copy()
    fi, fo = min(int(fade_in * SR), len(a) // 2), min(int(fade_out * SR), len(a) // 2)
    if fi:
        a[:fi] *= np.linspace(0.0, 1.0, fi)
    if fo:
        a[-fo:] *= np.linspace(1.0, 0.0, fo)
    peak = np.abs(a).max()
    if peak > 0:
        a *= 10 ** (-1.0 / 20) / peak
    return a


def write(a, cue, name):
    folder = os.path.join(OUT, cue)
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, name + ".wav")
    pcm = np.clip(np.round(a * 32767), -32768, 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())
    return path


def takes(src, folder):
    out = []
    for f in sorted(os.listdir(os.path.join(src, folder))):
        stem, ext = os.path.splitext(f)
        if ext.lower() != ".wav" or stem in SKIP:
            continue
        a = trim(load(os.path.join(src, folder, f)))
        peak_db = 20 * np.log10(np.abs(a).max() + 1e-12) if len(a) else -120
        if len(a) < 0.2 * SR or peak_db < -20:
            print(f"NEONSOUNDS skip {stem}: {len(a) / SR:.2f} s, peak {peak_db:.1f} dB")
            continue
        out.append((stem, a))
    return out


def lovin_takes(clips, count=3, seconds=28.0, seed=6650):
    """The lovin sound is one long take played once (docs/design/sound.md, "Lovin"): string the short pleasure sounds
    together with 1.5 to 4 s of quiet between, a little softer or louder each time."""
    rng = random.Random(seed)
    made = []
    for _ in range(count):
        parts, length = [], 0.0
        order = clips[:]
        rng.shuffle(order)
        i = 0
        while length < seconds:
            stem, a = order[i % len(order)]
            i += 1
            gap = np.zeros(int(rng.uniform(1.5, 4.0) * SR))
            parts += [gap, finish(a) * 10 ** (rng.uniform(-6.0, 0.0) / 20)]
            length += (len(gap) + len(a)) / SR
        made.append(finish(np.concatenate(parts + [np.zeros(int(0.3 * SR))]), fade_in=0.0, fade_out=0.3))
    return made


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    src = sys.argv[1]
    written = {}

    def put(cue, name, a, **fade):
        write(finish(a, **fade), cue, name)
        written[cue] = written.get(cue, 0) + 1

    male = takes(src, "male_gruff_set1") + takes(src, "male_gruff_set2")
    for stem, a in male:
        cue = "Voice_MaleHit" if len(a) / SR <= HIT_MAX else "Voice_MaleDowned"
        put(cue, "neon_" + stem.replace("male_gruff_", "m"), a)
    for stem, a in takes(src, "female_pain"):
        cue = "Voice_FemHit" if len(a) / SR <= HIT_MAX else "Voice_FemDowned"
        put(cue, "neon_" + stem, a)
    for stem, a in takes(src, "female_begging"):
        put("Voice_FemDowned", "neon_" + stem, a)
    for n, a in enumerate(lovin_takes(takes(src, "female_pleasure")), 1):
        write(a, "Voice_LovinFem", f"neon_pleasure_take{n}")
        written["Voice_LovinFem"] = written.get("Voice_LovinFem", 0) + 1
    for file, cues in LYRIA.items():
        a = load(os.path.join(src, file))
        for cue, windows in cues.items():
            for n, (start, seconds) in enumerate(windows, 1):
                piece = a[int(start * SR):int((start + seconds) * SR)]
                put(cue, f"neon_{n:02d}", piece, fade_in=0.02, fade_out=0.15 if not cue.endswith("Death") else 0.4)
    for cue, n in sorted(written.items()):
        print(f"NEONSOUNDS {cue}: {n} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
