#!/bin/bash
# Run one authentication through the daemon and check the final status.
# Usage: e2e.sh <scenario name> <expected status prefix> [start args...]
# Environment: ADDR (bus address), USER_NAME (default: current user),
#              NOPOLL (seconds to wait before the first GetResult)
name=$1
expected=$2
shift 2

call() {
	busctl --address="$ADDR" call org.boltgolt.howdy.authentication /org/boltgolt/howdy/Authentication \
		org.boltgolt.howdy.Authentication "$@" 2>&1
}

out=$(call StartAuthentication sas "${USER_NAME:-$(id -un)}" $# "$@")
if [[ "$out" != s* ]]; then
	res="$out"
else
	id=$(echo "$out" | cut -d'"' -f2)
	start=${EPOCHREALTIME/[.,]/}
	if [ -n "${NOPOLL:-}" ]; then
		sleep "$NOPOLL"
	fi
	while :; do
		res=$(call GetResult s "$id" | cut -d'"' -f2)
		[ "$res" != RUNNING ] && break
		sleep 0.3
	done
	# EPOCHREALTIME has a decimal comma in some locales, count microseconds
	elapsed=$(( ${EPOCHREALTIME/[.,]/} - start ))
	printf -v elapsed '%d.%d' $((elapsed / 1000000)) $(((elapsed % 1000000) / 100000))
	res="$res (${elapsed}s)"
fi

if [[ "$res" == "$expected"* ]]; then
	echo "PASS  $name: $res"
else
	echo "FAIL  $name: got '$res', expected '$expected'"
	exit 1
fi
