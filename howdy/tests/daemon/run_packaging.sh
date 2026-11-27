#!/bin/bash
# Test of what runs during an installation: the script of "meson install" that puts
# the default config. Nothing is built.
set -uo pipefail

REPOSITORY="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
WORK_DIR="${WORK_DIR:-$(mktemp -d -t howdy-packaging-tests.XXXXXX)}"
mkdir -p "$WORK_DIR"
echo "Work directory: $WORK_DIR"

failed=0
expect() {
	if [ "$2" = "$3" ]; then
		echo "PASS  $1"
	else
		echo "FAIL  $1: got '$2', expected '$3'"
		failed=1
	fi
}

echo "--- the default config of meson install"
CONFIG_SCRIPT="$REPOSITORY/howdy/assets/config/install-config.sh"
rm -rf "$WORK_DIR/config"
mkdir -p "$WORK_DIR/config"
echo default > "$WORK_DIR/config/default.ini"
DESTDIR="$WORK_DIR/config/root" sh "$CONFIG_SCRIPT" "$WORK_DIR/config/default.ini" /etc/howdy > /dev/null
INSTALLED="$WORK_DIR/config/root/etc/howdy/config.ini"
expect "installed where there is none" "$(cat "$INSTALLED")" default
expect "readable for everybody" "$(stat -c %a "$INSTALLED")" 644
echo changed > "$INSTALLED"
DESTDIR="$WORK_DIR/config/root" sh "$CONFIG_SCRIPT" "$WORK_DIR/config/default.ini" /etc/howdy > /dev/null
expect "a config that is there is kept" "$(cat "$INSTALLED")" changed
MESON_INSTALL_DESTDIR_PREFIX="$WORK_DIR/config/prefix" sh "$CONFIG_SCRIPT" "$WORK_DIR/config/default.ini" etc/howdy > /dev/null
expect "a relative directory is below the prefix" "$(cat "$WORK_DIR/config/prefix/etc/howdy/config.ini")" default

exit $failed
