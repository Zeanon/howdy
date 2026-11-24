#!/bin/bash
# A private D-Bus for the tests, sourced after common.sh. The system bus is not
# needed, neither is root.
#
# Provides: ADDR (also DBUS_SYSTEM_BUS_ADDRESS, so the components connect to
# it), register_service (D-Bus activation), start_bus, start_background
# (processes that are stopped at the end), wait_for_name, expect and the failed
# counter.

for tool in dbus-daemon busctl; do
	command -v "$tool" > /dev/null || { echo "Missing $tool"; exit 1; }
done

# The socket is abstract, filesystem paths are limited to 108 characters
SOCK="howdy-test-$$"
export ADDR="unix:abstract=$SOCK"
export DBUS_SYSTEM_BUS_ADDRESS="$ADDR"

BACKGROUND_PIDS=()
stop_background() {
	local pid
	for pid in "${BACKGROUND_PIDS[@]}"; do
		kill "$pid" 2> /dev/null || true
		# (a process that was stopped by a test only ends when it goes on)
		kill -CONT "$pid" 2> /dev/null || true
	done
	wait 2> /dev/null || true
}
trap stop_background EXIT

# Run a command in the background until the end of the test, sets BACKGROUND_PID
start_background() {
	"$@" >> "$T/background.log" 2>&1 &
	BACKGROUND_PID=$!
	BACKGROUND_PIDS+=("$BACKGROUND_PID")
}

wait_for_name() {
	local _
	for _ in $(seq 150); do
		busctl --address="$ADDR" status "$1" > /dev/null 2>&1 && return
		sleep 0.2
	done
	echo "Nobody owns $1, see $T/background.log"
	exit 1
}

# Let the bus start a program when its name is used, like the .service files of
# the system bus. Has to be called before start_bus.
mkdir -p "$T/services"
register_service() {
	printf '[D-BUS Service]\nName=%s\nExec=%s\n' "$1" "$2" > "$T/services/$1.service"
}

start_bus() {
	cat > "$T/bus.conf" <<CONF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
	<type>session</type>
	<listen>$ADDR</listen>
	<auth>EXTERNAL</auth>
	<servicedir>$T/services</servicedir>
	<policy context="default">
		<allow user="*"/>
		<allow own="*"/>
		<allow send_destination="*" eavesdrop="true"/>
		<allow receive_sender="*"/>
		<allow eavesdrop="true"/>
	</policy>
</busconfig>
CONF
	start_background dbus-daemon --config-file="$T/bus.conf" --nofork --nosyslog
	sleep 0.5
}

failed=0
# expect <name> <actual> <expected prefix>
expect() {
	if [[ "$2" == "$3"* ]]; then echo "PASS  $1: $2"; else echo "FAIL  $1: got '$2', expected '$3'"; failed=1; fi
}
# expect_contains <name> <actual> <expected substring>
expect_contains() {
	if [[ "$2" == *"$3"* ]]; then echo "PASS  $1"; else echo "FAIL  $1: got '$2', expected to contain '$3'"; failed=1; fi
}
# expect_missing <name> <actual> <text that must not be in it>
expect_missing() {
	if [[ "$2" != *"$3"* ]]; then echo "PASS  $1"; else echo "FAIL  $1: found '$3'"; failed=1; fi
}
