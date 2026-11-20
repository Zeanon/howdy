#!/bin/bash
# Test of the checks who calls the Python daemons (authentication and management
# daemon, polkit check of the cli package), without a bus, polkit or a camera.
# Needs a Python with dbus-next, opencv and numpy.
# See common.sh for HOWDY_PYTHON and WORK_DIR.
set -euo pipefail

TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOWDY_PYTHON="${HOWDY_PYTHON:-/usr/local/lib/x86_64-linux-gnu/Howdy/.venv/bin/python3}"
WORK_DIR="${WORK_DIR:-$(mktemp -d -t howdy-python-tests.XXXXXX)}"
mkdir -p "$WORK_DIR"

"$HOWDY_PYTHON" -I "$TEST_DIR/python_callers.py" "$TEST_DIR/../../src/python" "$WORK_DIR"
