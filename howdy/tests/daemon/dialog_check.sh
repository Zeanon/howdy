#!/bin/bash
# Looks at the enrollment dialog from the outside, like a person (and the
# window manager) would. Runs inside a virtual display: xvfb-run -a dialog_check.sh ...
#
#   dialog_check.sh transient <enroll_test>
#       Starts the program, which makes a window for the dialog to belong to, and
#       prints that window and what the dialog says about itself.
#   dialog_check.sh cancel <program and arguments>
#       Starts the program, clicks "Cancel" in its dialog and prints how it
#       exited, then what it said.
set -uo pipefail
# The title of the dialog has a dash that is not ASCII, xdotool needs a UTF-8 locale to find it
export LC_ALL=C.UTF-8

mode=$1
shift

# The dialog has no decoration and no focus, it is found by its name
find_dialog() {
	local id
	for _ in $(seq 150); do
		id="$(xdotool search --name "Create Model" 2> /dev/null | head -1)"
		if [ -n "$id" ]; then
			echo "$id"
			return 0
		fi
		sleep 0.2
	done
	return 1
}

log="$(mktemp)"
trap 'rm -f "$log"; kill "${pid:-0}" 2> /dev/null || true' EXIT

# The dialog never appeared: say what the program said
no_dialog() {
	echo "no dialog, the program said:"
	cat "$log"
	exit 1
}

case "$mode" in
	transient)
		"$1" --with-parent > "$log" 2>&1 &
		pid=$!
		dialog="$(find_dialog)" || no_dialog
		# The program says which window it made for the dialog
		parent="$(grep -oE 'parent: 0x[0-9a-f]+' "$log" | cut -d' ' -f2)"
		echo "parent: $parent"
		echo "dialog: $(xprop -id "$dialog" WM_TRANSIENT_FOR 2>&1)"
		wait "$pid"
		;;
	cancel)
		"$@" > "$log" 2>&1 &
		pid=$!
		dialog="$(find_dialog)" || no_dialog
		# The button is at the bottom, where exactly depends on the theme: try
		# until the dialog reacts. Clicks that miss the button do nothing.
		for y in 570 560 580 550 590 540 600 530 520 510 500 490 480 470; do
			kill -0 "$pid" 2> /dev/null || break
			xdotool mousemove --window "$dialog" 250 "$y" click 1
			sleep 0.1
		done
		code=0
		wait "$pid" || code=$?
		echo "exit code: $code"
		cat "$log"
		;;
	*)
		echo "usage: dialog_check.sh transient|cancel ..."
		exit 2
		;;
esac
