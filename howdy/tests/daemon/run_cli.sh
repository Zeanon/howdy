#!/bin/bash
# Test of the C++ management daemon and the C++ cli over a private D-Bus. A
# stand-in for polkit answers the authorization requests, the camera is a
# video made from a still image. No root and no system bus access is needed.
#
#   SLOW_TESTS=1   additionally runs `howdy add` with the real timings
#                  (about a minute) and waits for the idle timeout of the
#                  management daemon
#
# See common.sh for the environment variables.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/bus.sh"

for tool in ffmpeg unshare; do
	command -v "$tool" > /dev/null || { echo "Missing $tool"; exit 1; }
done
HAS_XVFB=0
command -v xvfb-run > /dev/null && HAS_XVFB=1

link_management "$T/management"
link_cli "$T/howdy"
link "$T/enroll_test" "${BASE_SOURCES[@]}" "${CORE_SOURCES[@]}" "$SRC_DIR/cli/enroll_dialog.cc" "$TEST_DIR/enroll_test.cc"

USER_NAME="$(id -un)"
MODEL="$T/models/$USER_NAME.dat"
HOWDY="$T/howdy"
export TZ=UTC

# --- Test data

rm -f "$T/models"/*.dat
make_test_videos

write_config() {
	"$HOWDY_PYTHON" -I "$TEST_DIR/mkconf.py" "$CONFIG_TEMPLATE" "$T/etc/config.ini" \
		"video.device_path=$1" video.recording_plugin=ffmpeg video.device_format=mov
}
write_config "$T/still.mp4"

# --- Stand-in for polkit and the management daemon

# The management daemon is started by the bus on the first call, and again after
# it stopped because it was idle, like on the system bus
register_service org.boltgolt.howdy.management "$T/management"
start_bus

echo allow > "$T/polkit.mode"
: > "$T/polkit.log"
start_background "$HOWDY_PYTHON" -I "$TEST_DIR/fake_polkit.py" "$T/polkit.mode" "$T/polkit.log"
wait_for_name org.freedesktop.PolicyKit1

mcall() {
	busctl --address="$ADDR" call org.boltgolt.howdy.management /org/boltgolt/howdy/Management org.boltgolt.howdy.Management "$@" 2>&1 || true
}

MODEL_JSON='{"time":1700000000,"label":"first","data":[[0.1,0.2,0.3]]}'
SECOND_JSON='{"time":1700000100,"label":"second","data":[[0.4,0.5,0.6]]}'

# Run the cli, the output and the exit code are in $output and $code
run() {
	code=0
	output=$("$@" 2>&1) || code=$?
}

echo "--- management daemon"
# The daemon creates the directory when the first model is saved
rm -rf "$T/models"
expect "list without models" "$(mcall List s "$USER_NAME")" 's "[]"'
expect "add" "$(mcall Add ss "$USER_NAME" "$MODEL_JSON")" "b true"
# Only root can look into the directory, so nobody else can tell who has a model
expect "the models directory is only for root" "$(stat -c %a "$T/models")" 700
expect "model file mode" "$(stat -c %a "$MODEL")" 600
expect "add as the caller, without a user" "$(mcall Add s "$SECOND_JSON")" "b true"
out="$(mcall List "")"
expect_contains "list has the first model" "$out" 'id\":0'
expect_contains "list has the second model" "$out" 'id\":1'
expect_contains "list has the labels" "$out" 'label\":\"second'
expect "list without a user" "$(mcall List)" 's "[{'
if grep -q '"data"' <<< "$out"; then echo "FAIL  list leaks the encodings"; failed=1; else echo "PASS  list does not leak the encodings"; fi
# The daemon compares a face with the models, so that howdy test needs no access to them
expect_contains "compare finds the closest model" "$(mcall Compare ssad "$USER_NAME" euclidean 3 0.1 0.2 0.5)" '"first"'
expect_contains "compare gives the distance" "$(mcall Compare sad euclidean 3 0.1 0.2 0.3)" "bds true 0 \"first\""
expect_contains "compare with the dot product takes the largest" "$(mcall Compare sad dot 3 1 1 1)" '"second"'
expect "compare skips models of another size" "$(mcall Compare sad euclidean 2 0.1 0.2)" 'bds false 0 ""'
expect_contains "compare refuses an unknown metric" "$(mcall Compare sad cosine 3 0.1 0.2 0.3)" "Unknown metric cosine"
expect_contains "compare refuses an empty face" "$(mcall Compare sad euclidean 0)" "Invalid face encoding"
expect_contains "compare for another user is refused" "$(mcall Compare ssad root euclidean 3 0.1 0.2 0.3)" "Not authorized"
# It is called for every frame: polkit is asked on the first call of a connection only
asked() { grep -c "org.boltgolt.howdy.Management" "$T/polkit.log" || true; }
before="$(asked)"
out="$("$HOWDY_PYTHON" -I "$TEST_DIR/compare_calls.py" 4 euclidean 0.1 0.2 0.3)"
expect "compare on one connection finds the model every time" "$(grep -c "first" <<< "$out")" 4
expect "polkit is asked once for four calls" "$(($(asked) - before))" 1
# The keep-alive, like the one of the authentication daemon: two seconds after polkit said yes, and
# every call of the connection extends it to one second from then
expect "KeepAlive of a connection that never compared is not kept" "$(mcall KeepAlive)" "b false"
before="$(asked)"
out="$(COMPARE_PAUSE=1.5 "$HOWDY_PYTHON" -I "$TEST_DIR/compare_calls.py" 3 euclidean 0.1 0.2 0.3)"
expect "compare finds the model after a pause" "$(grep -c "first" <<< "$out")" 3
expect "polkit is asked again when the keep-alive ran out (two seconds, then one)" "$(($(asked) - before))" 2
before="$(asked)"
out="$(COMPARE_PAUSE=2.5 "$HOWDY_PYTHON" -I "$TEST_DIR/compare_calls.py" 2 euclidean 0.1 0.2 0.3)"
expect "polkit is asked again after more than two seconds without a call" "$(($(asked) - before))" 2
before="$(asked)"
out="$(COMPARE_PAUSE=2.5 COMPARE_KEEPALIVE=0.5 "$HOWDY_PYTHON" -I "$TEST_DIR/compare_calls.py" 2 euclidean 0.1 0.2 0.3)"
expect "KeepAlive pings are answered with true" "$(grep -c "keepalive True" <<< "$out")" 5
expect "and polkit is asked once for the two calls" "$(($(asked) - before))" 1
# A refusal is not remembered, the next connection asks again
echo deny > "$T/polkit.mode"
before="$(asked)"
out="$("$HOWDY_PYTHON" -I "$TEST_DIR/compare_calls.py" 3 euclidean 0.1 0.2 0.3)"
expect "polkit can refuse compare" "$(grep -c "org.boltgolt.howdy.Management.NotAuthorized" <<< "$out")" 3
expect "a refusal is asked again every time" "$(($(asked) - before))" 3
echo allow > "$T/polkit.mode"
expect "remove" "$(mcall Remove ss "$USER_NAME" 0)" "b true"
expect_contains "removed model is gone" "$(mcall List s "$USER_NAME")" 'id\":1'
expect "remove unknown id" "$(mcall Remove ss "$USER_NAME" 9)" "Call failed: No model with ID 9 exists for $USER_NAME"
expect "remove the last model" "$(mcall Remove s 1)" "b true"
expect "file is removed with the last model" "$([ -e "$MODEL" ] && echo exists || echo gone)" gone
expect "remove without models" "$(mcall Remove ss "$USER_NAME" 0)" "Call failed: No face model known for user $USER_NAME"
expect "clear without models" "$(mcall Clear s "$USER_NAME")" "Call failed: $USER_NAME has no models or they have already been cleared"
mcall Add s "$MODEL_JSON" > /dev/null
expect "clear" "$(mcall Clear)" "b true"
expect "clear removed the file" "$([ -e "$MODEL" ] && echo exists || echo gone)" gone

echo "--- management daemon, untrusted model file"
# The models decide who is let in, a file that others can write is not used
mcall Add s "$MODEL_JSON" > /dev/null
chmod 666 "$MODEL"
expect_contains "list refuses a model file that others can write" "$(mcall List)" "Not using"
expect_contains "add refuses it" "$(mcall Add s "$SECOND_JSON")" "Not using"
expect_contains "remove refuses it" "$(mcall Remove s 0)" "Not using"
chmod 600 "$MODEL"
expect "list works again with the right mode" "$(mcall List)" 's "[{'
mcall Clear > /dev/null

echo "--- management daemon, invalid input"
expect "ids continue after the last one" "$(mcall Add s "$MODEL_JSON")" "b true"
expect "invalid json" "$(mcall Add s 'not json')" "Call failed: Model is not valid JSON"
expect "model is not an object" "$(mcall Add s '[]')" "Call failed: Model must be a dictionary"
expect "model without time" "$(mcall Add s '{"label":"x","data":[]}')" "Call failed: Model has no valid time"
expect "model without label" "$(mcall Add s '{"time":1,"data":[]}')" "Call failed: Model has no valid label"
expect "model with text as encoding" "$(mcall Add s '{"time":1,"label":"x","data":[["a"]]}')" "Call failed: Model has no valid data"
expect "label too long" "$(mcall Add s "{\"time\":1,\"label\":\"$(printf 'x%.0s' $(seq 300))\",\"data\":[]}")" "Call failed: Model label is too long"
expect "too many encodings" "$(mcall Add s "{\"time\":1,\"label\":\"x\",\"data\":[$(printf '[1],%.0s' $(seq 64))[1]]}")" "Call failed: Model has too many encodings"
expect "encoding too long" "$(mcall Add s "{\"time\":1,\"label\":\"x\",\"data\":[[$(printf '1,%.0s' $(seq 512))1]]}")" "Call failed: Model has no valid data"
expect "wrong arguments" "$(mcall Add i 5)" "Call failed: Invalid arguments for Add"
expect_contains "unknown method" "$(mcall Frobnicate)" "Call failed"
mcall Clear > /dev/null
for _ in $(seq 64); do mcall Add s "$MODEL_JSON" > /dev/null; done
expect "too many models" "$(mcall Add s "$MODEL_JSON")" "Call failed: Too many face models, please remove one first"
expect "the limit is not reached by removing" "$(mcall Remove s 0)" "b true"
expect "a model can be added again" "$(mcall Add s "$MODEL_JSON")" "b true"
mcall Clear > /dev/null
expect "other user" "$(mcall List s nobody)" "Call failed: Not authorized to modify models for user nobody"
expect "unknown user" "$(mcall List s nosuchuser_x)" "Call failed: Unknown user: nosuchuser_x"
expect "path as user" "$(mcall List s ../x)" "Call failed: Unknown user: ../x"
mcall Clear > /dev/null

echo "--- polkit"
echo deny > "$T/polkit.mode"
expect "denied" "$(mcall List s "$USER_NAME")" "Call failed: Polkit authorization denied"
echo challenge > "$T/polkit.mode"
expect "needs authentication" "$(mcall List s "$USER_NAME")" "Call failed: Polkit authorization denied (authentication required)"
echo allow > "$T/polkit.mode"
expect "allowed again" "$(mcall List s "$USER_NAME")" 's "[]"'
# The client is checked as system-bus-name, for the management action, with user interaction
expect_contains "polkit was asked for the right action" "$(cat "$T/polkit.log")" "system-bus-name :"
if grep -qE "^system-bus-name :[0-9.]+ org\.boltgolt\.howdy\.Management 1$" "$T/polkit.log"; then
	echo "PASS  polkit call has the action and allows user interaction"
else
	echo "FAIL  polkit call: $(tail -1 "$T/polkit.log")"
	failed=1
fi

echo "--- polkit, the error that clients can tell apart"
# A refused authorization has an error name of its own, other failures have the one of the method
if command -v gdbus > /dev/null; then
	gcall() {
		gdbus call --address "$ADDR" --dest org.boltgolt.howdy.management --object-path /org/boltgolt/howdy/Management \
			--method "org.boltgolt.howdy.Management.$1" "${@:2}" 2>&1 || true
	}
	echo deny > "$T/polkit.mode"
	expect_contains "a refusal by polkit" "$(gcall List "'$USER_NAME'")" "org.boltgolt.howdy.Management.NotAuthorized: Polkit authorization denied"
	echo allow > "$T/polkit.mode"
	expect_contains "somebody else's models" "$(gcall List "'nobody'")" "org.boltgolt.howdy.Management.NotAuthorized: Not authorized to modify models for user nobody"
	expect_contains "other failures keep the name of the method" "$(gcall Remove "'$USER_NAME'" "'9'")" "org.boltgolt.howdy.Management.Remove.Error: No face model known"
else
	echo "gdbus not found, skipping the check of the error names"
fi

echo "--- a call that is cancelled"
# howdy test gives its call up when the window is closed, while the daemon may wait for a polkit
# that asks for a password. The client has to be free in a short time.
link "$T/cancel_test" "${BASE_SOURCES[@]}" "$TEST_DIR/cancel_test.cc"
mcall Add s "$MODEL_JSON" > /dev/null
echo slow > "$T/polkit.mode"
out="$("$T/cancel_test" "$USER_NAME" cancel)"
expect_contains "a call that waits for polkit is cancelled" "$out" "cancelled "
ms="${out##* }"
if [ "${ms:-99999}" -lt 1500 ]; then echo "PASS  in a short time ($ms ms, polkit needs 5000)"; else echo "FAIL  it took $ms ms"; failed=1; fi
out="$("$T/cancel_test" "$USER_NAME" wait)"
expect_contains "a call that is not cancelled gets its answer" "$out" "answered first "
echo allow > "$T/polkit.mode"
out="$("$T/cancel_test" "$USER_NAME" error)"
expect_contains "and an error of the daemon comes through" "$out" "error Unknown metric cosine"
mcall Clear > /dev/null

echo "--- cli"
run "$HOWDY" version
expect "version" "$output" "Howdy test"
run "$HOWDY" -h
expect "help" "$output" "usage: howdy [-U USER] [--plain] [-h] [-y] command [arguments...]"
run "$HOWDY"
expect "no arguments" "$output" "current active user: $USER_NAME"
run "$HOWDY" frobnicate
expect "invalid command" "$code" 2
expect_contains "invalid command message" "$output" "invalid choice: 'frobnicate'"
run "$HOWDY" -U root list
expect_contains "no commands as root" "$output" "Can't run howdy commands as root"

run "$HOWDY" list
expect "list without models, exit code" "$code" 1
expect_contains "list without models" "$output" "No face model known for the user $USER_NAME, please run:"
run "$HOWDY" list --plain
expect "list without models, plain" "$output" ""

mcall Add s "$MODEL_JSON" > /dev/null
mcall Add s "$SECOND_JSON" > /dev/null
run "$HOWDY" list --plain
expect "list, plain" "$output" "0,2023-11-14 22:13:20,first
1,2023-11-14 22:15:00,second"
run "$HOWDY" list
expect_contains "list header" "$output" "Known face models for $USER_NAME:"
expect_contains "list row" "$output" "0   2023-11-14 22:13:20  first"

run "$HOWDY" remove
expect_contains "remove without id" "$output" "Please add the ID of the model you want to remove as an argument"
run "$HOWDY" remove 7 -y
expect "remove unknown id, exit code" "$code" 1
expect_contains "remove unknown id" "$output" "No model with ID 7 exists for $USER_NAME"
run bash -c "echo n | '$HOWDY' remove 0"
expect_contains "remove declined" "$output" 'This will remove the model called "first"'
expect_contains "remove declined, aborted" "$output" 'Interpreting as a "NO", aborting'
run "$HOWDY" remove 0 -y
expect "remove" "$output" "Removed model 0"
run bash -c "echo y | '$HOWDY' remove 1"
expect_contains "remove last model, confirmed" "$output" "Removed last model, howdy disabled for user"

mcall Add s "$MODEL_JSON" > /dev/null
run bash -c "echo n | '$HOWDY' clear"
expect_contains "clear declined" "$output" 'Interpreting as a "NO", aborting'
expect "clear declined, model is kept" "$([ -e "$MODEL" ] && echo exists || echo gone)" exists
run "$HOWDY" clear -y
expect_contains "clear" "$output" "Models cleared"
run "$HOWDY" clear -y
expect "clear without models, exit code" "$code" 1
expect_contains "clear without models" "$output" "has no models or they have already been cleared"

run "$HOWDY" -U nobody list
expect "other user" "$output" "Authorization failed: Not authorized to modify models for user nobody"
echo deny > "$T/polkit.mode"
run "$HOWDY" list
expect "polkit denies" "$output" "Authorization failed: Polkit authorization denied"
echo allow > "$T/polkit.mode"

echo "--- cli, config"
cp "$CONFIG_TEMPLATE" "$T/etc/config.ini"
chmod 640 "$T/etc/config.ini"
run "$HOWDY" disable 1
expect_contains "disable needs root" "$output" "Please run this command as root:"
run "$HOWDY" set certainty 80
expect_contains "set needs root" "$output" "sudo howdy set certainty 80"
# A user namespace is root for the cli, without being root
as_root() { unshare -r "$@"; }
run as_root "$HOWDY" disable 1
expect "disable" "$output" "Howdy has been disabled"
expect_contains "config is disabled" "$(grep '^disabled' "$T/etc/config.ini")" "disabled = true"
run as_root "$HOWDY" disable true
expect "disable twice" "$output" "The disable option has already been set to true"
run as_root "$HOWDY" disable 0
expect "enable" "$output" "Howdy has been enabled"
expect_contains "config is enabled" "$(grep '^disabled' "$T/etc/config.ini")" "disabled = false"
run as_root "$HOWDY" disable maybe
expect "disable, invalid argument" "$output" "Please only use 0 (enable) or 1 (disable) as an argument"
expect "config permissions are kept" "$(stat -c %a "$T/etc/config.ini")" 640
run as_root "$HOWDY" set certainty 80
expect "set" "$output" "Config option updated"
expect_contains "config has the new value" "$(grep '^certainty' "$T/etc/config.ini")" "certainty = 80"
expect "set keeps the other lines" "$(diff <(sed 's/^certainty = .*//;s/^disabled = .*//' "$CONFIG_TEMPLATE") <(sed 's/^certainty = .*//;s/^disabled = .*//' "$T/etc/config.ini") && echo same)" same
run as_root "$HOWDY" set nosuchoption 1
expect "set, unknown option" "$output" 'Could not find a "nosuchoption" config option to set'
run as_root "$HOWDY" set certainty
expect_contains "set without a value" "$output" "Please add a setting you would like to change and the value to set it to"

# The editor and systemctl are stand-ins, the real service must not be restarted
mkdir -p "$T/bin"
printf '#!/bin/sh\necho "$@" >> "%s/systemctl.log"\n' "$T" > "$T/bin/systemctl"
printf '#!/bin/sh\necho "# edited" >> "$1"\n' > "$T/bin/fake-editor"
chmod +x "$T/bin/systemctl" "$T/bin/fake-editor"
: > "$T/systemctl.log"
run env EDITOR="$T/bin/fake-editor" PATH="$T/bin:$PATH" unshare -r "$HOWDY" config
expect "config opens the editor" "$output" "Opening config.ini in fake-editor"
expect "config was edited" "$(tail -1 "$T/etc/config.ini")" "# edited"
expect "config restarts the authentication" "$(cat "$T/systemctl.log")" "restart howdy-authentication-test.service"

echo "--- cli, snapshot"
write_config "$T/still.mp4"
run "$HOWDY" snapshot
expect_contains "snapshot" "$output" "Generated snapshot saved as"
SNAPSHOT="$(tail -1 <<< "$output")"
expect "snapshot file mode" "$(stat -c %a "$SNAPSHOT")" 600
expect "snapshot directory mode" "$(stat -c %a "$(dirname "$SNAPSHOT")")" 700
# 4 frames next to each other, 4 lines of text below
expect "snapshot size" "$("$HOWDY_PYTHON" -I -c "import cv2; print(cv2.imread('$SNAPSHOT').shape[:2])")" "(600, 1440)"
write_config /dev/does-not-exist
run "$HOWDY" snapshot
expect "snapshot without camera, exit code" "$code" 13
expect_contains "snapshot without camera" "$output" "Howdy could not find a camera device"
write_config "$T/still.mp4"

if [ "$HAS_XVFB" = 1 ]; then
	echo "--- enrollment dialog (xvfb)"
	write_config "$T/single.mp4"
	run xvfb-run -a "$T/enroll_test" "$T/genuine.dat"
	expect_contains "samples are captured" "$output" "samples: "
	expect_contains "the user is guided" "$output" "say: Turn your head slightly to the left"
	distance="$(grep -oE 'distance: [0-9.e-]+' <<< "$output" | cut -d' ' -f2)"
	expect "the first sample is the face" "$(awk "BEGIN { print (${distance:-9} < 0.4) ? \"close\" : \"far\" }")" close

	# The dialog belongs to the window of the program that asked for it
	run xvfb-run -a "$TEST_DIR/dialog_check.sh" transient "$T/enroll_test"
	parent="$(grep -oE 'parent: 0x[0-9a-f]+' <<< "$output" | cut -d' ' -f2)"
	expect_contains "the dialog belongs to the window that asked for it" "$output" "WM_TRANSIENT_FOR(WINDOW): window id # $parent"
	# The user closes the dialog: a frontend can tell it from a failure
	run xvfb-run -a "$TEST_DIR/dialog_check.sh" cancel "$HOWDY" add -y -- "cancelled"
	expect_contains "closing the dialog has an exit code of its own" "$output" "exit code: 3"
	run xvfb-run -a "$TEST_DIR/dialog_check.sh" cancel "$T/enroll_test"
	expect_contains "the enrollment reports the cancellation" "$output" "exit code: 1"

	write_config "$T/blank.mp4"
	run xvfb-run -a "$T/enroll_test"
	expect_contains "no face" "$output" "error: No face detected, aborting"
	write_config /dev/does-not-exist
	run xvfb-run -a "$T/enroll_test"
	expect_contains "enrollment without camera" "$output" "error: Howdy could not find a camera device"
	write_config "$T/still.mp4"

	echo "--- cli, test window (xvfb)"
	mcall Add s "$MODEL_JSON" > /dev/null
	cp "$T/genuine.dat" "$MODEL"
	# The window never closes by itself, stop it after a few seconds
	asked_before="$(asked)"
	run xvfb-run -a timeout 12 "$HOWDY" test
	expect_contains "test shows the detection" "$output" "Opening a window with a test feed"
	expect_contains "test prints the texture" "$output" "texture: "
	expect_contains "test prints the movement" "$output" "Micro-movement: "
	expect_contains "test compares the face" "$output" "similarity: "
	expect_missing "test compares through the daemon" "$output" "Could not compare"
	# The cli asks before the window opens, the daemon once for the connection, not for every frame
	expect "polkit is asked twice during the test" "$(($(asked) - asked_before))" 2
	# The password is asked before the camera is opened: the polkit prompt may start a face
	# authentication (pam_howdy), which needs the camera. So a refusal comes before any camera error.
	write_config /dev/does-not-exist
	echo deny > "$T/polkit.mode"
	run xvfb-run -a timeout 12 "$HOWDY" test
	expect_contains "test asks polkit before it opens the camera" "$output" "Authorization failed: Polkit authorization denied"
	expect_missing "and does not look for the camera before" "$output" "camera"
	echo allow > "$T/polkit.mode"
	write_config "$T/still.mp4"
	mcall Clear > /dev/null
else
	echo "xvfb-run not found, skipping the tests that need a display"
fi

if [ "${SLOW_TESTS:-0}" = 1 ]; then
	if [ "$HAS_XVFB" = 1 ]; then
		echo "--- cli, add (xvfb, real timings)"
		write_config "$T/single.mp4"
		run xvfb-run -a "$HOWDY" add -y "my face"
		expect_contains "add" "$output" "Added a new model to $USER_NAME"
		expect_contains "the model is listed" "$("$HOWDY" list --plain)" ",my face"
		expect "the model is stored by the daemon" "$(stat -c %a "$MODEL")" 600
		"$HOWDY" clear -y > /dev/null
	fi

	echo "--- management daemon, idle timeout"
	mcall List s "$USER_NAME" > /dev/null
	expect "daemon was started by the bus" "$(pgrep -f "^$T/management" > /dev/null && echo running || echo stopped)" running
	sleep 33
	expect "daemon stops after the idle timeout" "$(pgrep -f "^$T/management" > /dev/null && echo running || echo stopped)" stopped
	expect "daemon is started again by the next call" "$(mcall List s "$USER_NAME")" 's "[]"'
fi

echo
[ "$failed" = 0 ] && echo "All tests passed" || echo "Some tests FAILED, logs in $T"
exit $failed
