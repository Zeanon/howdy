#!/bin/sh
# Run by "meson install": puts the default config where there is none. It is decided at the time
# of the installation, a config that is there is never overwritten.
#   install-config.sh <default config> <config directory>
set -eu

case "$2" in
	/*) directory="${DESTDIR:-}$2" ;;
	*) directory="$MESON_INSTALL_DESTDIR_PREFIX/$2" ;;
esac

if [ -e "$directory/config.ini" ] || [ -L "$directory/config.ini" ]; then
	echo "Keeping the config $directory/config.ini"
	exit 0
fi

echo "Installing $1 to $directory"
mkdir -p -- "$directory"
install -m 0644 -- "$1" "$directory/config.ini"
