#!/usr/bin/env bash
# Run a command with the pinned Zephyr set up, from any shell: ./zephyr.sh west build ...
# Like idf.sh for the ESP32 firmware; build.sh radar uses it.
set -euo pipefail
source "$(dirname "$0")/zephyr-env.sh"

if [[ ! -x $ZEPHYR_WS/.venv/bin/west || ! -d $ZEPHYR_SDK_INSTALL_DIR ]]; then
  echo "Zephyr $ZEPHYR_VERSION isn't installed; run esp32/zephyr-setup.sh first" >&2
  exit 1
fi
source "$ZEPHYR_WS/.venv/bin/activate"
export PATH=$ZEPHYR_SDK_INSTALL_DIR/hosttools/bin:$PATH        # dtc
[[ -n $JLINK_DIR ]] && export PATH=$JLINK_DIR:$PATH
exec "$@"
