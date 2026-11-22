#!/usr/bin/env bash
# Make Rim3D assets with Google's Gemini 4 Neon (Antigravity CLI "agy") on sneed's PC, headless. Git Bash.
#   MODEL=gemini-4-neon-high OUT=<folder> JOBS=3 tools/neon_assets/run_neon_assets.sh [Name ...]
# Each <Name>.md here is a full brief (tools/codex_assets/_common.md + the asset + its animation part).
# A job whose folder already has contact_sheet.jpg is skipped. Logs: OUT/<Name>/neon.log (JSON with token stats).
# MODEL can be any agy model id (agy models), e.g. claude-opus-5-5 for the Opus test. Usage: agy -p "/usage".
set -u
export MSYS_NO_PATHCONV=1
HERE="$(cd "$(dirname "$0")" && pwd)"
AGY="${AGY:-$LOCALAPPDATA/agy/bin/agy.exe}"
OUT="${OUT:-/c/Users/thill/OneDrive/Documents/Rimworld3D/neon_test}"
JOBS="${JOBS:-3}"
MODEL="${MODEL:-gemini-4-neon-high}"
names=("$@")
if [ ${#names[@]} -eq 0 ]; then for f in "$HERE"/*.md; do b="$(basename "$f" .md)"; [ "$b" = README ] || names+=("$b"); done; fi
run_one() {
  local name="$1" dir="$OUT/$1"
  mkdir -p "$dir"
  if [ -f "$dir/contact_sheet.jpg" ]; then echo "skip $name (done)"; return; fi
  cp "$HERE/$name.md" "$dir/BRIEF.md"
  echo "start $name $(date +%H:%M:%S)" | tee -a "$dir/neon_times.txt"
  ( cd "$dir" && "$AGY" --model "$MODEL" --sandbox --dangerously-skip-permissions --output-format json -p \
    "Read BRIEF.md in this folder and make the asset it describes, start to finish, fully autonomously (nobody will answer questions). The asset name is $name. Work only inside this folder. Run Blender only in background mode: blender -b --factory-startup -P script.py (Blender is under C:\\Program Files\\Blender Foundation\\). Deliver every file the brief lists, including the previews, preview_anim.gif, contact_sheet.jpg and README.md, and do the self-critique improvement pass." \
    </dev/null > neon.log 2> neon.err.log )
  echo "end $name $(date +%H:%M:%S) exit $? sheet=$([ -f "$dir/contact_sheet.jpg" ] && echo yes || echo NO)" | tee -a "$dir/neon_times.txt"
}
for n in "${names[@]}"; do
  while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n; done
  run_one "$n" & sleep 5
done
wait; echo "all done $(date +%H:%M:%S)"
