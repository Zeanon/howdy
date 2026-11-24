#!/bin/bash
# Test of read_trusted_file(), which decides if a model file is used, and of is_privileged(), which
# decides who may compare faces. No camera or bus needed. See common.sh for the environment variables.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

link "$WORK_DIR/trusted_file_test" "${BASE_SOURCES[@]}" "$TEST_DIR/trusted_file_test.cc"
link "$WORK_DIR/caller_test" "${BASE_SOURCES[@]}" "$TEST_DIR/caller_test.cc"
status=0
"$WORK_DIR/trusted_file_test" || status=1
"$WORK_DIR/caller_test" || status=1
exit "$status"
