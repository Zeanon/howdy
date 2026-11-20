#!/bin/bash
# Test of read_trusted_file(), which decides if a model file is used. No
# camera or bus needed. See common.sh for the environment variables.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

link "$WORK_DIR/trusted_file_test" "${BASE_SOURCES[@]}" "$TEST_DIR/trusted_file_test.cc"
"$WORK_DIR/trusted_file_test"
