#!/usr/bin/env bash
# Make Rim3D assets with the Codex CLI, one job per brief, a few at a time. Git Bash on sneed's PC.
#   CODEX_MODEL=<model id> OUT=<folder> JOBS=3 tools/codex_assets/run_codex_assets.sh [Name ...]
# With no names it runs every <Name>.md here except _common.md. A job whose folder already has
# contact_sheet.jpg is skipped (delete it to remake). Each job gets its own folder OUT/<Name> holding
# BRIEF.md (the shared brief + the asset's brief), codex.log and what Codex makes.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-/c/Users/thill/OneDrive/Documents/ChatGPT/Models/Rim3D_Codex}"
JOBS="${JOBS:-3}"
MODEL_ARGS=()
[ -n "${CODEX_MODEL:-}" ] && MODEL_ARGS=(-m "$CODEX_MODEL")

names=("$@")
if [ ${#names[@]} -eq 0 ]; then
  for f in "$HERE"/*.md; do b="$(basename "$f" .md)"; case "$b" in _common|README) ;; *) names+=("$b") ;; esac; done
fi

run_one() {
  local name="$1" dir="$OUT/$1"
  mkdir -p "$dir"
  if [ -f "$dir/contact_sheet.jpg" ]; then echo "skip $name (done)"; return; fi
  { cat "$HERE/_common.md"; echo; echo "---"; echo; sed "s/<Name>/$name/g" "$HERE/$name.md"; } > "$dir/BRIEF.md"
  echo "start $name $(date +%H:%M:%S)"
  codex exec --skip-git-repo-check --sandbox workspace-write "${MODEL_ARGS[@]}" -C "$dir" \
    "Read BRIEF.md in this folder and make the asset it describes, start to finish, fully autonomously (nobody will answer questions). The asset name is $name. Deliver every file the brief lists, including the previews, contact_sheet.jpg and README.md, and do the self-critique improvement pass." \
    </dev/null > "$dir/codex.log" 2>&1
  echo "end $name $(date +%H:%M:%S) exit $? sheet=$([ -f "$dir/contact_sheet.jpg" ] && echo yes || echo NO)"
}

for n in "${names[@]}"; do
  while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n; done
  run_one "$n" &
  sleep 5
done
wait
echo "all done $(date +%H:%M:%S)"
