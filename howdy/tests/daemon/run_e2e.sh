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
# The thresholds come from the config file, the daemon reads it once at startup
start_daemon "${NOLIVE[@]}" face_authentication.certainty=99
E "genuine, strict certainty=99 in the config" TIMEOUT
start_daemon "${NOLIVE[@]}"
E "invalid argument" ERROR timeout=abc
E "a threshold can not be set by an argument" SUCCESS certainty=1 confirmations=99 anti-spoof-threshold=0
# The ffmpeg reader scales the picture to the size of the config, here only the width is set
start_daemon "${NOLIVE[@]}" video.frame_width=400
E "genuine face, scaled picture" SUCCESS
start_daemon "${NOLIVE[@]}"
cp "$T/impostor.dat" "$MODEL"
E "foreign face" TIMEOUT
USER_NAME=nosuchuser E "unknown user" "Call failed: Unknown user"
NOPOLL=4 E "client stops polling" KEEPALIVE_TIMEOUT timeout=0

echo "--- busy / cancel / clear"
id=$(call StartAuthentication sas "$USER_NAME" 1 timeout=0 | cut -d'"' -f2)
expect "second start while running" "$(call StartAuthentication sas "$USER_NAME" 0)" "Call failed: An authentication is already running"
USER_NAME=nobody E "a user without a model is answered while a scan is running" NO_ENROLLED_FACE
sleep 0.5
expect "running" "$(call GetResult s "$id")" 's "RUNNING"'
expect "cancel" "$(call CancelAuthentication s "$id")" "b true"
# The daemon says CANCELLING until the scan has ended, that takes longer on a slow computer: waited for up to 10 seconds
for _ in $(seq 100); do
	# (a call that fails is the failure of the check below, not the end of the tests)
	result=$(call GetResult s "$id") || true
	[ "$result" = 's "CANCELLING"' ] || break
	sleep 0.1
done
expect "cancelled" "$result" 's "CANCELLED"'
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
# UpdateMessage is left out: the text of Show is sent again as a sign of life
members=$(grep -oE "member=(Show|UpdateSubtext|Close)" "$T/monitor.log" | uniq | cut -d= -f2 | tr '\n' ' ' || true)
expect "popup signal order" "$members" "Show UpdateSubtext Close"

echo "--- untrusted model file"
# The models decide who is let in, a file that others can change is not used
start_daemon "${NOLIVE[@]}"
cp "$T/genuine.dat" "$MODEL"
chmod 666 "$MODEL"
E "model writable by everybody" ERROR
chmod 660 "$MODEL"
E "model writable by the group" ERROR
chmod 644 "$MODEL"
E "model readable by others" SUCCESS
chmod 600 "$MODEL"
E "model of the owner only" SUCCESS
mv "$MODEL" "$T/genuine-link.dat"
ln -s "$T/genuine-link.dat" "$MODEL"
E "model is a link" ERROR
rm "$MODEL"
cp "$T/genuine.dat" "$MODEL"
chmod 600 "$MODEL"

echo "--- snapshot retention"
# A snapshot of an earlier time is deleted when the daemon starts, of the failed
# attempts only the newest ones are kept
rm -rf "$T/log/snapshots"
mkdir -p "$T/log/snapshots"
touch -d "30 days ago" "$T/log/snapshots/20200101T000000.jpg"
start_daemon "${NOLIVE[@]}" snapshots.save_failed=true snapshots.max_snapshots=2 snapshots.retention=7d
expect "an old snapshot is deleted when the daemon starts" "$(find "$T/log/snapshots" -name '*.jpg' | wc -l)" 0
cp "$T/impostor.dat" "$MODEL"
for _ in 1 2 3; do E "foreign face, the snapshot is saved" TIMEOUT; done
expect "only the newest two snapshots are kept" "$(find "$T/log/snapshots" -name '*.jpg' | wc -l)" 2

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
	# A face that does not move but has the noise of a camera is no nod and no shake, the stamp waits
	# (the stamp waits for exactly one face, the picture shows two people)
	"$HOWDY_PYTHON" -I "$TEST_DIR/crop_face.py" "$T/openvino.json" "$IMG_DIR/2008_001009.jpg" "$T/single.jpg"
	ffmpeg -loglevel error -y -loop 1 -i "$T/single.jpg" -t 8 -r 15 -pix_fmt yuv420p -vf noise=alls=6:allf=t -crf 0 "$T/noisy.mp4"
	start_daemon "${NOLIVE[@]}" "video.device_path=$T/noisy.mp4" face_authentication.use_openvino=true video.openvino_device=CPU \
		face_authentication.use_anti_spoof=false face_authentication.use_damo_flir=false \
		face_authentication.micromotion_detection=true face_authentication.micromotion_threshold=0 \
		rubberstamps.enabled=true "rubberstamps.stamp_rules=nod 4s failsafe"
	started=$SECONDS
	E "openvino nod: a still face does not confirm" NO_MATCH
	expect "and does not abort either" "$([ $((SECONDS - started)) -ge 4 ] && echo yes || echo "$((SECONDS - started)) s")" yes
fi

# A caller without privileges that was told that an authentication is running is told so again without the
# daemon reading the model once more (the PAM module asks every 250 ms while it waits), until that one is over.
# The daemon runs as another user for this, in a user namespace, so that the tests are a caller without
# privileges.
echo "--- a caller without privileges while an authentication runs"
if unshare --user --map-user=12345 true 2> /dev/null; then
	"$HOWDY_PYTHON" -I "$TEST_DIR/mkconf.py" "$CONFIG_TEMPLATE" "$T/etc/config.ini" "${BASE[@]}" "${NOLIVE[@]}" \
		face_authentication.timeout=8 face_authentication.start_cooldown=0
	kill "$DPID"
	wait "$DPID" 2> /dev/null || true
	start_background unshare --user --map-user=12345 "$T/daemon"
	DPID=$BACKGROUND_PID
	wait_for_name org.boltgolt.howdy.authentication
	cp "$T/impostor.dat" "$MODEL"
	chmod 600 "$MODEL"
	expect "the tests are a caller without privileges" "$(call StartAuthentication sas nobody 0)" "Call failed: Only the own user can be authenticated"
	id=$(call StartAuthentication sas "$USER_NAME" 0 | cut -d'"' -f2)
	expect "a second start while one runs" "$(call StartAuthentication sas "$USER_NAME" 0)" "Call failed: An authentication is already running"
	# Without the model the answer would be that there is none, if the daemon looked for it again
	rm -f "$MODEL"
	expect "asked again: busy, the model is not read again" "$(call StartAuthentication sas "$USER_NAME" 0)" "Call failed: An authentication is already running"
	expect "the first one still runs" "$(call GetResult s "$id")" 's "RUNNING"'
	expect "cancel the first one" "$(call CancelAuthentication s "$id")" "b true"
	for _ in $(seq 100); do
		result=$(call GetResult s "$id") || true
		[ "$result" = 's "CANCELLING"' ] || break
		sleep 0.1
	done
	expect "the first one is over" "$result" 's "CANCELLED"'
	id=$(call StartAuthentication sas "$USER_NAME" 0 | cut -d'"' -f2)
	expect "then the caller is looked at again: no model" "$(call GetResult s "$id")" 's "NO_ENROLLED_FACE"'
	# It can not keep the camera for as long as it wants: no timeout it asks for is longer than the one of the
	# config or 60 seconds (the face does not match here, so the scan ends with the timeout)
	cp "$T/impostor.dat" "$MODEL"
	chmod 600 "$MODEL"
	id=$(call StartAuthentication sas "$USER_NAME" 1 timeout=-1 | cut -d'"' -f2)
	started=$SECONDS
	result=""
	while [ $((SECONDS - started)) -lt 75 ]; do
		result=$(call GetResult s "$id") || true
		[ "$result" = 's "RUNNING"' ] || break
		sleep 0.3
	done
	expect "no timeout of a caller without privileges ends all the same" "$result" 's "TIMEOUT"'
	expect "after a minute" "$([ $((SECONDS - started)) -ge 55 ] && [ $((SECONDS - started)) -le 70 ] && echo yes || echo "$((SECONDS - started)) s")" yes
else
	echo "SKIPPING: no user namespace with another user"
fi

echo "--- a rubber stamp rule with a typo"
start_daemon "${NOLIVE[@]}" rubberstamps.enabled=true "rubberstamps.stamp_rules=nodd 5s faildeadly"
cp "$T/genuine.dat" "$MODEL"
chmod 600 "$MODEL"
E "a stamp that is not installed denies" NO_MATCH
# All rules are read first: the nod (20 seconds) is not asked for
start_daemon "${NOLIVE[@]}" rubberstamps.enabled=true $'rubberstamps.stamp_rules=nod 20s failsafe\nnodd 5s faildeadly'
started=$SECONDS
E "a later rule that is not valid denies" NO_MATCH
expect "before the first stamp runs" "$([ $((SECONDS - started)) -le 10 ] && echo yes || echo "$((SECONDS - started)) s")" yes
start_daemon "${NOLIVE[@]}" rubberstamps.enabled=true "rubberstamps.stamp_rules=nod 20s faildeadly min_distance"
E "an argument without = denies" NO_MATCH
start_daemon "${NOLIVE[@]}" rubberstamps.enabled=true "rubberstamps.stamp_rules=nod 20s faildeadly min_directons=4"
E "an option the stamp does not have denies" NO_MATCH
start_daemon "${NOLIVE[@]}" rubberstamps.enabled=true "rubberstamps.stamp_rules=nod 20s faildeadly min_distance=5x"
E "a value that is no number denies" NO_MATCH
start_daemon "${NOLIVE[@]}" rubberstamps.enabled=true "rubberstamps.stamp_rules=nod 1.2.3s faildeadly"
E "a timeout that is no number denies" NO_MATCH

# A unit that is not known is not read as seconds, minutes are minutes
start_daemon "${NOLIVE[@]}" rubberstamps.enabled=true "rubberstamps.stamp_rules=nod 1x faildeadly"
E "a timeout with an unknown unit denies" NO_MATCH
start_daemon "${NOLIVE[@]}" rubberstamps.enabled=true "rubberstamps.stamp_rules=nod 0.05m faildeadly"
started=$SECONDS
E "a timeout in minutes" SUCCESS
expect "lasts that long" "$([ $((SECONDS - started)) -ge 3 ] && echo yes || echo "$((SECONDS - started)) s")" yes

echo "--- several people in the picture"
# Each of them can log in, the checks that compare the frames look at one person only
ffmpeg -loglevel error -y -loop 1 -i "$IMG_DIR/2008_002079.jpg" -t 3 -r 15 -pix_fmt yuv420p -vf "scale=trunc(iw/2)*2:trunc(ih/2)*2" "$T/crowd.mp4"
for face in 0 2; do
	"$HOWDY_PYTHON" -I "$TEST_DIR/mkmodel.py" "$T/hog.json" 2008_002079.jpg "$face" "$MODEL"
	chmod 600 "$MODEL"
	start_daemon "${NOLIVE[@]}" "video.device_path=$T/crowd.mp4" video.max_height=2000 \
		face_authentication.similarity_detection=true face_authentication.confirmations=2
	E "face $face of several, with the similarity check" SUCCESS
done
cp "$T/genuine.dat" "$MODEL"
chmod 600 "$MODEL"

echo "--- the popup stays while a rubber stamp waits"
start_daemon "${NOLIVE[@]}" rubberstamps.enabled=true "rubberstamps.stamp_rules=nod 4s failsafe"
dbus-monitor --address "$ADDR" "type='signal',interface='org.boltgolt.howdy.AuthPopup'" > "$T/monitor.log" 2>&1 &
MPID=$!
sleep 0.5
E "a nod that does not come, with popup" NO_MATCH show-auth-popup
sleep 0.5
kill "$MPID"
# The longest time without a signal while the stamp ran, the popup hides itself after one second
gap=$(awk '/member=(Show|UpdateMessage|UpdateSubtext|Close)/ {
	match($0, /time=[0-9.]+/)
	now = substr($0, RSTART + 5, RLENGTH - 5)
	if (stamp) { if (last != "" && now - last > longest) longest = now - last; last = now }
} /string "[^"]*[Nn]od/ { stamp = 1 }
END { if (!stamp) print "no stamp"; else print (longest < 0.9 ? "short" : longest) }' "$T/monitor.log")
expect "no long pause between the signals" "$gap" short

echo "--- a client that goes while a rubber stamp waits"
start_daemon "${NOLIVE[@]}" rubberstamps.enabled=true "rubberstamps.stamp_rules=nod 10s failsafe"
NOPOLL=5 E "the stamp is given up" KEEPALIVE_TIMEOUT
# The camera is free again, the next one does not get a busy answer
E "the next authentication runs" NO_MATCH

echo "--- a camera that delivers nothing"
# ffmpeg is a stand-in that never ends, the daemon gives it up
mkdir -p "$T/hang"
printf '#!/bin/sh\nexec sleep 60\n' > "$T/hang/ffmpeg"
chmod +x "$T/hang/ffmpeg"
"$HOWDY_PYTHON" -I "$TEST_DIR/mkconf.py" "$CONFIG_TEMPLATE" "$T/etc/config.ini" "${BASE[@]}" "${NOLIVE[@]}"
kill "$DPID"
wait "$DPID" 2> /dev/null || true
start_background env PATH="$T/hang:$PATH" "$T/daemon"
DPID=$BACKGROUND_PID
wait_for_name org.boltgolt.howdy.authentication
cp "$T/genuine.dat" "$MODEL"
chmod 600 "$MODEL"
started=$SECONDS
NOPOLL= E "a camera that delivers nothing: given up" NO_DEVICE
expect "after some seconds" "$([ $((SECONDS - started)) -le 25 ] && echo yes || echo "$((SECONDS - started)) s")" yes

echo "--- shutdown"
kill "$DPID"
rc=0
wait "$DPID" || rc=$?
expect "exit code after SIGTERM" "$rc" 0
DPID=""

echo
[ "$failed" = 0 ] && echo "All tests passed" || echo "Some tests FAILED, logs in $T"
exit $failed
