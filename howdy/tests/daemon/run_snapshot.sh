#!/bin/bash
# Test of the retention of the snapshots (age and number), without a camera
# or a bus. See common.sh for the environment variables.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

link "$WORK_DIR/snapshot_test" "${BASE_SOURCES[@]}" "$SRC_DIR/core/snapshot.cc" "$TEST_DIR/snapshot_test.cc"
"$WORK_DIR/snapshot_test"
