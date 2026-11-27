#!/bin/bash
# Test of the hotkey rubber stamp with keyboards that are stand-ins, without
# root, a camera or a bus. See common.sh for the environment variables.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

SOURCES=("$SRC_DIR/common/util.cc" "$SRC_DIR/core/rubberstamps/hotkey.cc" "$SRC_DIR/core/rubberstamps/stamp.cc" "$TEST_DIR/hotkey_test.cc")
build_objects "${SOURCES[@]}"
objects=()
for source in "${SOURCES[@]}"; do
	objects+=("$(object_path "$source")")
done
"$CXX" "${SANITIZER_LINK[@]}" "${objects[@]}" -o "$WORK_DIR/hotkey_test" -Wl,--wrap=open "${PKG_LIBS[@]}" -lpthread
"$WORK_DIR/hotkey_test"
