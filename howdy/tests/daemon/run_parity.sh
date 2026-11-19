#!/bin/bash
# Compare detections, encodings and liveness/spoof scores of the C++ daemon
# with the Python implementation on the dlib example images.
# See common.sh for the environment variables.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

link "$WORK_DIR/parity" "${BASE_SOURCES[@]}" "${CORE_SOURCES[@]}" "$TEST_DIR/parity.cc"

I="$IMG_DIR"
SMALL=("$I/2007_007763.jpg" "$I/2008_001009.jpg" "$I/2008_001322.jpg" "$I/2008_002079.jpg")
MODES=(hog cnn)
[ "$HAS_OPENVINO" = 1 ] && MODES+=(openvino)

failed=0
for mode in "${MODES[@]}"; do
	images=("${SMALL[@]}")
	# The CNN detector is too slow on the large image without a GPU
	[ "$mode" != cnn ] && images+=("$I/bald_guys.jpg")

	echo "== $mode"
	"$HOWDY_PYTHON" -I "$TEST_DIR/parity.py" "$WORK_DIR/pyhowdy" "$mode" "${images[@]}" > "$WORK_DIR/py_$mode.json"
	"$WORK_DIR/parity" "$mode" "${images[@]}" > "$WORK_DIR/cc_$mode.json"
	"$HOWDY_PYTHON" -I "$TEST_DIR/compare.py" "$WORK_DIR/py_$mode.json" "$WORK_DIR/cc_$mode.json" || failed=1
done

exit $failed
