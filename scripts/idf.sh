#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFAULT_IDF_DIR="$HOME/.espressif/v5.4.3/esp-idf"
FALLBACK_IDF_DIR="$ROOT_DIR/../esp/esp-idf"
IDF_DIR="${IDF_PATH:-$DEFAULT_IDF_DIR}"

if [[ ! -f "$IDF_DIR/export.sh" && -f "$FALLBACK_IDF_DIR/export.sh" ]]; then
    IDF_DIR="$FALLBACK_IDF_DIR"
fi

if [[ ! -f "$IDF_DIR/export.sh" ]]; then
    echo "ESP-IDF export.sh introuvable: $IDF_DIR/export.sh" >&2
    exit 1
fi

PREFERRED_PY_ENV="$HOME/.espressif/python_env/idf5.4_py3.14_env"
if [[ "$IDF_DIR" == "$DEFAULT_IDF_DIR" && -x "$PREFERRED_PY_ENV/bin/python" ]]; then
    export IDF_PYTHON_ENV_PATH="$PREFERRED_PY_ENV"
    export PATH="$PREFERRED_PY_ENV/bin:$PATH"
fi

# shellcheck disable=SC1090
source "$IDF_DIR/export.sh" >/dev/null

exec idf.py "$@"
