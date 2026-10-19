# The pinned Zephyr, its SDK and libsodium for the radar firmware (FRDM-RW612), and where they live.
# Sourced by zephyr-setup.sh and zephyr.sh; override any of these in the environment.
ZEPHYR_VERSION=${ZEPHYR_VERSION:-v4.4.2}
ZEPHYR_SDK_VERSION=${ZEPHYR_SDK_VERSION:-1.0.1}       # the one zephyr/SDK_VERSION names
LIBSODIUM_VERSION=${LIBSODIUM_VERSION:-1.0.22}        # the same as the ESP32 builds' espressif/libsodium
ZEPHYR_WS=${ZEPHYR_WS:-$HOME/zephyr/zephyr-$ZEPHYR_VERSION}                    # west workspace
export ZEPHYR_BASE=$ZEPHYR_WS/zephyr
export ZEPHYR_SDK_INSTALL_DIR=${ZEPHYR_SDK_INSTALL_DIR:-$HOME/zephyr/zephyr-sdk-$ZEPHYR_SDK_VERSION}
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export LIBSODIUM_DIR=${LIBSODIUM_DIR:-$ZEPHYR_WS/libsodium}
# SEGGER's J-Link tools, for flashing through the board's J-Link (zephyr-setup.sh says where to get them)
JLINK_DIR=${JLINK_DIR:-$(ls -d "$HOME"/zephyr/JLink_Linux_* /opt/SEGGER/JLink 2>/dev/null | tail -1 || true)}
