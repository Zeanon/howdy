#!/bin/bash
# Test of what runs during an installation: the scripts of the Debian package with
# another root directory (DPKG_ROOT) and a stand-in for pam-auth-update, and the
# script of "meson install" that puts the default config. Nothing is built.
set -uo pipefail

REPOSITORY="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
WORK_DIR="${WORK_DIR:-$(mktemp -d -t howdy-packaging-tests.XXXXXX)}"
mkdir -p "$WORK_DIR/bin"
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

if ! command -v dpkg > /dev/null; then
	echo "SKIP  there is no dpkg, the scripts of the Debian package are not tested"
	exit $failed
fi

# As root a script that does not keep to DPKG_ROOT would change the models of this computer
if [ "$(id -u)" = 0 ]; then
	echo "SKIP  run as root, the scripts of the Debian package are only tested without root"
	exit $failed
fi

printf '#!/bin/sh\nexit 0\n' > "$WORK_DIR/bin/pam-auth-update"
chmod +x "$WORK_DIR/bin/pam-auth-update"
ROOT="$WORK_DIR/root"
OLD_ETC="$ROOT/etc/howdy/models"
OLD_LIB="$ROOT/lib/security/howdy/models"
NEW="$ROOT/var/lib/howdy/models"

# A system with models of older packages, readable for everybody as they were left
old_system() {
	rm -rf "$ROOT"
	mkdir -p "$OLD_ETC" "$OLD_LIB" "$ROOT/var/lib/howdy"
	echo etc > "$OLD_ETC/anna.dat"
	echo lib > "$OLD_LIB/anna.dat"
	echo lib > "$OLD_LIB/ben.dat"
	chmod 755 "$OLD_ETC" "$OLD_LIB" "$OLD_ETC"/*.dat "$OLD_LIB"/*.dat
}
configure() {
	PATH="$WORK_DIR/bin:$PATH" DPKG_ROOT="$ROOT" sh "$REPOSITORY/debian/postinst" configure "$1" > "$WORK_DIR/postinst.log" 2>&1 ||
		{ echo "FAIL  postinst failed"; cat "$WORK_DIR/postinst.log"; failed=1; }
}
files() { (cd "$ROOT" && find etc lib var -name '*.dat' -printf '%p %m\n' 2> /dev/null | sort | tr '\n' '|'); }

echo "--- upgrade from 2.x"
old_system
configure 2.6.1
expect "the models are moved, the one in /etc goes first" "$(cat "$NEW/anna.dat") $(cat "$NEW/ben.dat")" "etc lib"
expect "another model of a user stays, only for root" "$(files)" \
	"lib/security/howdy/models/anna.dat 600|var/lib/howdy/models/anna.dat 600|var/lib/howdy/models/ben.dat 600|"
expect "its directory is only for root" "$(stat -c %a "$OLD_LIB")" 700
expect "an empty directory is removed" "$([ -e "$OLD_ETC" ] && echo there || echo gone)" gone

echo "--- upgrade from 3.0.2"
old_system
configure 3.0.2
expect "what was left in /lib does not come back" "$([ -e "$NEW/ben.dat" ] && echo there || echo gone)" gone
expect "the one in /etc is moved" "$(cat "$NEW/anna.dat")" etc

echo "--- no upgrade from an old package"
old_system
configure 3.0.3
expect "nothing is moved" "$([ -e "$NEW" ] && ls "$NEW" | wc -l || echo 0)" 0

echo "--- an old directory that is a link to the new one, and the other way round"
for direction in old new file; do
	rm -rf "$ROOT"
	mkdir -p "$ROOT/etc/howdy" "$ROOT/var/lib/howdy"
	case "$direction" in
		old)
			mkdir -p "$NEW"
			ln -s ../../var/lib/howdy/models "$OLD_ETC"
			echo model > "$NEW/anna.dat"
			;;
		new)
			mkdir -p "$OLD_ETC"
			ln -s ../../../etc/howdy/models "$NEW"
			echo model > "$OLD_ETC/anna.dat"
			;;
		file)
			mkdir -p "$OLD_ETC" "$NEW"
			echo model > "$OLD_ETC/anna.dat"
			ln -s ../../../../etc/howdy/models/anna.dat "$NEW/anna.dat"
			;;
	esac
	configure 3.0.2
	expect "the model is still there, link: $direction" "$(cat "$NEW/anna.dat" 2> /dev/null)" model
done

echo "--- a copy that breaks off"
old_system
# A full disk: the copy is cut off and fails
printf '#!/bin/sh\ncase "$*" in *-d*) exec %s "$@" ;; esac\neval "target=\\${$#}"\nprintf e > "$target"\nexit 1\n' "$(command -v install)" > "$WORK_DIR/bin/install"
chmod +x "$WORK_DIR/bin/install"
PATH="$WORK_DIR/bin:$PATH" DPKG_ROOT="$ROOT" sh "$REPOSITORY/debian/postinst" configure 3.0.2 > "$WORK_DIR/postinst.log" 2>&1
expect "postinst fails" "$([ $? = 0 ] && echo no || echo yes)" yes
expect "no model with a part of the content" "$([ -e "$NEW/anna.dat" ] && echo there || echo none)" none
rm "$WORK_DIR/bin/install"
configure 3.0.2
expect "the next time the whole model is moved" "$(cat "$NEW/anna.dat")" etc

echo "--- purge"
old_system
configure 2.6.1
DPKG_ROOT="$ROOT" sh "$REPOSITORY/debian/postrm" purge
expect "no face model is left" "$(files)" ""

exit $failed
