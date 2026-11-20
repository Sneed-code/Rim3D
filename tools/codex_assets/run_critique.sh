#!/usr/bin/env bash
# Second pass on finished assets: CRITIQUE.md beside BRIEF.md, then Codex improves the asset in place.
#   CODEX_MODEL=<id> OUT=<folder> tools/codex_assets/run_critique.sh Name [Name ...]   (all run at once)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-/c/Users/thill/OneDrive/Documents/ChatGPT/Models/Rim3D_Codex}"
MODEL_ARGS=(); [ -n "${CODEX_MODEL:-}" ] && MODEL_ARGS=(-m "$CODEX_MODEL")
for name in "$@"; do
  dir="$OUT/$name"
  cp "$HERE/critiques/$name.md" "$dir/CRITIQUE.md"
  mkdir -p "$dir/round1" && cp "$dir/contact_sheet.jpg" "$dir/round1/" 2>/dev/null
  ( echo "start $name $(date +%H:%M:%S)"
    codex exec --skip-git-repo-check --sandbox workspace-write "${MODEL_ARGS[@]}" -C "$dir" \
      "Read BRIEF.md and CRITIQUE.md in this folder. The asset $name already exists here with its build script; improve it to fix every point in CRITIQUE.md, fully autonomously (nobody will answer questions). Re-export the GLB, re-render every preview and contact_sheet.jpg, and update README.md." \
      </dev/null > "$dir/codex_round2.log" 2>&1
    echo "end $name $(date +%H:%M:%S) exit $?" ) &
  sleep 5
done
wait
echo "all done $(date +%H:%M:%S)"
