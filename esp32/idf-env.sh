# The pinned ESP-IDF and where it lives. Sourced by setup.sh and idf.sh; override any of these in the environment.
IDF_VERSION=${IDF_VERSION:-v6.0.3}
export IDF_PATH=${IDF_PATH:-$HOME/esp/esp-idf-$IDF_VERSION}
export IDF_TOOLS_PATH=${IDF_TOOLS_PATH:-$HOME/.espressif}
