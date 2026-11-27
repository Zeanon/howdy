#!/usr/bin/env bash
# Run every daemon, PAM, CLI and GUI test suite in sequence.
# WORK_DIR sets the parent directory for per-suite output; all output is kept.
set -uo pipefail

TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
RUN_DIR="${WORK_DIR:-$(mktemp -d -t howdy-all-tests.XXXXXX)}"
mkdir -p "$RUN_DIR"
RUN_DIR="$(cd -- "$RUN_DIR" && pwd)"
IMAGE_DIR="$RUN_DIR/images"
mkdir -p "$IMAGE_DIR"

SUITES=(
	run_trusted_file
	run_model_crypto
	run_snapshot
	run_hotkey
	run_python
	run_parity
	run_e2e
	run_cli
	run_gui
	run_pam
)
FAILED=()

echo "Test output: $RUN_DIR"
for suite in "${SUITES[@]}"; do
	SUITE_DIR="$RUN_DIR/$suite"
	mkdir -p "$SUITE_DIR"
	echo
	echo "===== $suite ====="
	if WORK_DIR="$SUITE_DIR" HOWDY_TEST_IMAGE_DIR="$IMAGE_DIR" "$TEST_DIR/$suite.sh" 2>&1 | tee "$SUITE_DIR/run.log"; then
		echo "===== PASS: $suite ====="
	else
		status=$?
		FAILED+=("$suite (exit $status)")
		echo "===== FAIL: $suite (exit $status) ====="
	fi
done

echo
if [ "${#FAILED[@]}" -eq 0 ]; then
	echo "All test suites passed. Results: $RUN_DIR"
	exit 0
fi

echo "Failed test suites:"
printf '  %s\n' "${FAILED[@]}"
echo "Full output: $RUN_DIR"
exit 1
