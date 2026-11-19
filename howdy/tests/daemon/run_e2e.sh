#!/bin/bash
# End-to-end test of the C++ daemon over a private D-Bus. A video made from
# a still image is used as camera through the ffmpeg recorder, no camera,
# root or system bus access is needed.
# See common.sh for the environment variables.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

source "$(dirname "${BASH_SOURCE[0]}")/bus.sh"
start_bus

for tool in dbus-monitor ffmpeg; do
	command -v "$tool" > /dev/null || { echo "Missing $tool"; exit 1; }
done

link_daemon "$T/daemon"

# Test data: the video and the models of a genuine and a foreign face
ffmpeg -loglevel error -y -loop 1 -i "$IMG_DIR/2008_001009.jpg" -t 3 -r 15 -pix_fmt yuv420p -vf scale=360:480 "$T/still.mp4"
"$HOWDY_PYTHON" -I "$TEST_DIR/parity.py" "$WORK_DIR/pyhowdy" hog "$IMG_DIR/2008_001009.jpg" "$IMG_DIR/2008_002079.jpg" > "$T/hog.json"
"$HOWDY_PYTHON" -I "$TEST_DIR/mkmodel.py" "$T/hog.json" 2008_001009.jpg 0 "$T/genuine.dat"
"$HOWDY_PYTHON" -I "$TEST_DIR/mkmodel.py" "$T/hog.json" 2008_002079.jpg 0 "$T/impostor.dat"

USER_NAME="$(id -un)"
export USER_NAME
MODEL="$T/models/$USER_NAME.dat"

BASE=(
	"video.device_path=$T/still.mp4"
	video.recording_plugin=ffmpeg
	video.device_format=mov
	face_authentication.timeout=3
)
NOLIVE=(
	face_authentication.micromotion_detection=false
	face_authentication.texture_detection=false
	face_authentication.similarity_detection=false
)

# The daemon reads the config once at startup
DPID=""
start_daemon() {
	"$HOWDY_PYTHON" -I "$TEST_DIR/mkconf.py" "$CONFIG_TEMPLATE" "$T/etc/config.ini" "${BASE[@]}" "$@"
	if [ -n "$DPID" ]; then
		kill "$DPID"
		wait "$DPID" 2>/dev/null || true
	fi
	start_background "$T/daemon"
	DPID=$BACKGROUND_PID
	wait_for_name org.boltgolt.howdy.authentication
}

call() {
	busctl --address="$ADDR" call org.boltgolt.howdy.authentication /org/boltgolt/howdy/Authentication \
		org.boltgolt.howdy.Authentication "$@" 2>&1
}

E() { "$TEST_DIR/e2e.sh" "$@" || failed=1; }

echo "--- dlib HOG, liveness disabled"
start_daemon "${NOLIVE[@]}"
rm -f "$MODEL"
E "no model" NO_ENROLLED_FACE
cp "$T/genuine.dat" "$MODEL"
E "genuine face" SUCCESS
E "genuine, strict certainty=99" TIMEOUT certainty=99
E "invalid argument" ERROR certainty=abc
cp "$T/impostor.dat" "$MODEL"
E "foreign face" TIMEOUT
USER_NAME=nosuchuser E "unknown user" "Call failed: Unknown user"
NOPOLL=4 E "client stops polling" KEEPALIVE_TIMEOUT timeout=0

echo "--- busy / cancel / clear"
id=$(call StartAuthentication sas "$USER_NAME" 1 timeout=0 | cut -d'"' -f2)
expect "second start while running" "$(call StartAuthentication sas "$USER_NAME" 0)" "Call failed: An authentication is already running"
sleep 0.5
expect "running" "$(call GetResult s "$id")" 's "RUNNING"'
expect "cancel" "$(call CancelAuthentication s "$id")" "b true"
sleep 0.5
expect "cancelled" "$(call GetResult s "$id")" 's "CANCELLED"'
expect "cancel again" "$(call CancelAuthentication s "$id")" "b false"
expect "clear" "$(call ClearAuthentication s "$id")" "b true"
expect "result after clear" "$(call GetResult s "$id")" "Call failed: Unknown authentication request"
expect "clear unknown" "$(call ClearAuthentication s nope)" "b false"

echo "--- popup signals"
cp "$T/genuine.dat" "$MODEL"
dbus-monitor --address "$ADDR" "type='signal',interface='org.boltgolt.howdy.AuthPopup'" > "$T/monitor.log" 2>&1 &
MPID=$!
sleep 0.5
E "genuine with popup" SUCCESS show-auth-popup
sleep 0.5
kill "$MPID"
members=$(grep -oE "member=(Show|UpdateMessage|UpdateSubtext|Close)" "$T/monitor.log" | uniq | cut -d= -f2 | tr '\n' ' ' || true)
expect "popup signal order" "$members" "Show UpdateMessage UpdateSubtext Close"

echo "--- dlib HOG, liveness enabled"
start_daemon
E "photo is rejected by liveness detection" TIMEOUT

echo "--- no camera"
start_daemon video.device_path=/dev/does-not-exist
E "missing device" NO_DEVICE

if [ "$HAS_OPENVINO" = 1 ]; then
	echo "--- OpenVINO CPU, liveness and anti-spoof disabled"
	"$HOWDY_PYTHON" -I "$TEST_DIR/parity.py" "$WORK_DIR/pyhowdy" openvino "$IMG_DIR/2008_001009.jpg" > "$T/openvino.json"
	"$HOWDY_PYTHON" -I "$TEST_DIR/mkmodel.py" "$T/openvino.json" 2008_001009.jpg 0 "$MODEL"
	start_daemon "${NOLIVE[@]}" face_authentication.use_openvino=true video.openvino_device=CPU \
		face_authentication.use_anti_spoof=false face_authentication.use_damo_flir=false
	E "openvino genuine face" SUCCESS
fi

echo "--- shutdown"
kill "$DPID"
rc=0
wait "$DPID" || rc=$?
expect "exit code after SIGTERM" "$rc" 0
DPID=""

echo
[ "$failed" = 0 ] && echo "All tests passed" || echo "Some tests FAILED, logs in $T"
exit $failed
