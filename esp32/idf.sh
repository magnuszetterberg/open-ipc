#!/usr/bin/env bash
# Run a command with the pinned ESP-IDF set up, from any shell: ./idf.sh idf.py build
# VS Code's tasks use this too, so the editor and the terminal build the same way.
set -euo pipefail
source "$(dirname "$0")/idf-env.sh"

if [[ ! -f $IDF_PATH/export.sh ]]; then
  echo "ESP-IDF $IDF_VERSION isn't installed; run esp32/setup.sh first" >&2
  exit 1
fi
log=$IDF_TOOLS_PATH/export.log              # export.sh is chatty; show it only when it fails
if ! source "$IDF_PATH/export.sh" >"$log" 2>&1; then
  cat "$log" >&2
  exit 1
fi
exec "$@"
