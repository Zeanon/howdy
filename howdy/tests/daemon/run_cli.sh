#!/bin/bash
# Test of the C++ management daemon and the C++ cli over a private D-Bus. A
# stand-in for polkit answers the authorization requests, the camera is a
# video made from a still image. No root and no system bus access is needed.
#
#   DAEMON_ONLY=1  only the management daemon over the bus and polkit (no cli, no camera
#                  stand-in, no models, no images, no ffmpeg): what the CI runs
#   SLOW_TESTS=1   additionally runs `howdy add` with the real timings
#                  (about a minute) and waits for the idle timeout of the
#                  management daemon
#
# See common.sh for the environment variables.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/bus.sh"

DAEMON_ONLY="${DAEMON_ONLY:-0}"
for tool in ffmpeg unshare; do
	[ "$DAEMON_ONLY" = 1 ] && continue
	command -v "$tool" > /dev/null || { echo "Missing $tool"; exit 1; }
done
HAS_XVFB=0
command -v xvfb-run > /dev/null && HAS_XVFB=1

link_management "$T/management"
if [ "$DAEMON_ONLY" != 1 ]; then
	link_cli "$T/howdy"
	link "$T/enroll_test" "${BASE_SOURCES[@]}" "${CORE_SOURCES[@]}" "$SRC_DIR/cli/enroll_dialog.cc" "$TEST_DIR/enroll_test.cc"
fi

# The second pass (see the end) runs as root in a user namespace, for the user that started the test
ROOT_PASS="${HOWDY_ROOT_PASS:-0}"
USER_NAME="${HOWDY_TEST_USER:-$(id -un)}"
MODEL="$T/models/$USER_NAME.dat"
HOWDY="$T/howdy"
export TZ=UTC

# --- Test data

rm -f "$T/models"/*.dat
[ "$DAEMON_ONLY" = 1 ] || make_test_videos

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

# The second pass: what only root may do (Compare of the daemon, howdy test), as root like in the
# installation. The daemon and the bus run in the user namespace as well, so that the daemon sees the uid 0.
if [ "$ROOT_PASS" = 1 ]; then
	source "$TEST_DIR/compare_as_root.sh"
	echo
	[ "$failed" = 0 ] && echo "All tests passed (as root)" || echo "Some tests FAILED (as root), logs in $T"
	exit "$failed"
fi

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
# The daemon compares a face with the models, so that howdy test needs no access to them. Only root may
# ask: a program of the user could work out the model from the answers. This pass runs as a normal user,
# which is refused (and nothing else is asked, not polkit either). The comparisons themselves are tested
# as root in compare_as_root.sh, in the second pass of this script (see the end).
asked() { grep -c "org.boltgolt.howdy.Management" "$T/polkit.log" || true; }
before="$(asked)"
expect_contains "compare is refused for a caller that is not root" "$(mcall Compare sad euclidean 3 0.1 0.2 0.3)" "Only root may compare faces"
expect_contains "also with the user name" "$(mcall Compare ssad "$USER_NAME" euclidean 3 0.1 0.2 0.3)" "Only root may compare faces"
expect_contains "and for another user, it is the first thing that is checked" "$(mcall Compare ssad root euclidean 3 0.1 0.2 0.3)" "Only root may compare faces"
echo allow > "$T/polkit.mode"
expect "polkit is not asked for it" "$(($(asked) - before))" 0
out="$("$HOWDY_PYTHON" -I "$TEST_DIR/compare_calls.py" 3 euclidean 0.1 0.2 0.3)"
expect "a program of the user is refused every time" "$(grep -c "Only root may compare\|NotAuthorized" <<< "$out")" 3
expect_contains "the models of the user can still be listed" "$(mcall List s "$USER_NAME")" 'label\":\"first'
expect_contains "KeepAlive does not exist any more" "$(mcall KeepAlive)" "Call failed"

expect "rename" "$(mcall Rename xsxs 1 second 1700000100 "Living room")" "b true"
expect_contains "the model has the new label" "$(mcall List s "$USER_NAME")" 'label\":\"Living room'
expect_contains "the other models keep theirs" "$(mcall List s "$USER_NAME")" 'label\":\"first'
expect "rename with a user" "$(mcall Rename sxsxs "$USER_NAME" 1 "Living room" 1700000100 "Office")" "b true"
expect "rename unknown id" "$(mcall Rename xsxs 9 x 1 y)" "Call failed: No model with ID 9 exists for $USER_NAME"
expect "rename with a label that is too long" "$(mcall Rename xsxs 1 Office 1700000100 "$(printf 'x%.0s' $(seq 300))")" "Call failed: Model label is too long"
expect "rename with an empty label" "$(mcall Rename xsxs 1 Office 1700000100 "")" "Call failed: Model label is empty"
expect "add with an empty label" "$(mcall Add s '{"time":1,"label":"","data":[]}')" "Call failed: Model label is empty"
expect "rename needs both arguments" "$(mcall Rename s 1)" "Call failed: Invalid arguments for Rename"
expect "rename with an old list" "$(mcall Rename xsxs 1 wrong 1700000100 x)" "Call failed: Model 1 has changed since the list was loaded, please reload the list"
expect "rename with another time" "$(mcall Rename xsxs 1 Office 5 x)" "Call failed: Model 1 has changed since the list was loaded, please reload the list"
expect "remove with an old list" "$(mcall Remove xsx 0 wrong 1700000000)" "Call failed: Model 0 has changed since the list was loaded, please reload the list"
expect "remove with another time" "$(mcall Remove xsx 0 first 5)" "Call failed: Model 0 has changed since the list was loaded, please reload the list"
expect "rename with a control character" "$(mcall Rename xsxs 1 Office 1700000100 $'a\x1b[31mb')" "Call failed: Model label has invalid characters"
expect "rename with a C1 control character" "$(mcall Rename xsxs 1 Office 1700000100 $'a\xc2\x9bb')" "Call failed: Model label has invalid characters"
expect "add with a control character" "$(mcall Add s '{"time":1,"label":"a\u001bb","data":[]}')" "Call failed: Model label has invalid characters"
expect_contains "the old list changed nothing" "$(mcall List s "$USER_NAME")" 'label\":\"first'
expect "remove" "$(mcall Remove sxsx "$USER_NAME" 0 first 1700000000)" "b true"
expect_contains "the model behind it moves up" "$(mcall List s "$USER_NAME")" 'id\":0'
expect "remove unknown id" "$(mcall Remove sxsx "$USER_NAME" 9 x 1)" "Call failed: No model with ID 9 exists for $USER_NAME"
expect "remove the last model" "$(mcall Remove xsx 0 Office 1700000100)" "b true"
expect "file is removed with the last model" "$([ -e "$MODEL" ] && echo exists || echo gone)" gone
expect "remove without models" "$(mcall Remove sxsx "$USER_NAME" 0 x 1)" "Call failed: No face model known for user $USER_NAME"
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
expect_contains "remove refuses it" "$(mcall Remove xsx 0 first 1700000000)" "Not using"
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
# Only what a model has is stored, other fields would have no limit
mcall Clear > /dev/null
mcall Add s '{"time":1,"label":"x","data":[[1]],"extra":"unlimited"}' > /dev/null
expect "other fields of a model are not stored" "$([ -s "$MODEL" ] && ! grep -q unlimited "$MODEL" && echo yes || echo no)" yes
expect_contains "unknown method" "$(mcall Frobnicate)" "Call failed"
mcall Clear > /dev/null
for _ in $(seq 64); do mcall Add s "$MODEL_JSON" > /dev/null; done
expect "too many models" "$(mcall Add s "$MODEL_JSON")" "Call failed: Too many face models, please remove one first"
expect "the limit is not reached by removing" "$(mcall Remove xsx 0 first 1700000000)" "b true"
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
	expect_contains "other failures keep the name of the method" "$(gcall Remove "'$USER_NAME'" "int64 9" "'x'" "int64 1")" "org.boltgolt.howdy.Management.Remove.Error: No face model known"
else
	echo "gdbus not found, skipping the check of the error names"
fi

# The second pass, as root in a user namespace (see compare_as_root.sh). Without user namespaces (the
# kernel or the system does not allow them to everybody) it is skipped, and what only root may do is
# not tested here: Compare is refused for a normal user above, but its answers are not checked.
second_pass() {
	echo "--- second pass, as root in a user namespace"
	if unshare -r true 2> /dev/null; then
		HOWDY_ROOT_PASS=1 HOWDY_TEST_USER="$USER_NAME" WORK_DIR="$WORK_DIR" DAEMON_ONLY="$DAEMON_ONLY" unshare -r bash "${BASH_SOURCE[0]}" || failed=1
	elif [ "${REQUIRE_ROOT_PASS:-0}" = 1 ]; then
		echo "FAIL  user namespaces are not available, but the second pass is required (REQUIRE_ROOT_PASS=1)"
		failed=1
	else
		echo "SKIP  user namespaces are not available, the tests of what only root may do did not run"
		second_pass_skipped=1
	fi
}

if [ "$DAEMON_ONLY" = 1 ]; then
	second_pass
	echo
	[ "$failed" = 0 ] && echo "All tests passed${second_pass_skipped:+, but the second pass (as root) was skipped}" || echo "Some tests FAILED, logs in $T"
	exit "$failed"
fi

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
# Started with sudo or doas the commands act for the user that did
for variable in SUDO_USER DOAS_USER; do
	run env -u SUDO_USER -u DOAS_USER -u PKEXEC_UID LOGNAME=root USER=root "$variable=$USER_NAME" "$HOWDY" list
	expect_contains "list with sudo or doas ($variable)" "$output" "Known face models for $USER_NAME:"
done
run env -u SUDO_USER -u DOAS_USER -u PKEXEC_UID LOGNAME=root USER=root "$HOWDY" list
expect_contains "root itself is still refused" "$output" "Can't run howdy commands as root"

run "$HOWDY" rename
expect_contains "rename without id" "$output" "Please add the ID of the model you want to rename as an argument"
run "$HOWDY" rename 1
expect "rename without a name can not ask here" "$code" 1
expect_contains "rename without a name" "$output" "Please add the new name of the model as an argument"
# Asked on a terminal only
run bash -c "echo 'Office, 2' | script -qec \"'$HOWDY' rename 1\" /dev/null"
expect_contains "rename asks for the name" "$output" "Enter a new label for this model [second]:"
expect_contains "rename with the answer" "$output" "Renamed model 1"
run "$HOWDY" list --plain
expect_contains "list shows the answer" "$output" "1,2023-11-14 22:15:00,Office 2"
run bash -c "echo | script -qec \"'$HOWDY' rename 1\" /dev/null"
expect_contains "an empty answer keeps the name" "$output" "Renamed model 1"
run "$HOWDY" list --plain
expect_contains "the name is kept" "$output" "1,2023-11-14 22:15:00,Office 2"
run "$HOWDY" rename 7 Kitchen
expect "rename unknown id, exit code" "$code" 1
expect_contains "rename unknown id" "$output" "No model with ID 7 exists for $USER_NAME"
run "$HOWDY" rename 1 "Living room"
expect "rename" "$output" "Renamed model 1"
run "$HOWDY" list --plain
expect "list shows the new name" "$output" "0,2023-11-14 22:13:20,first
1,2023-11-14 22:15:00,Living room"
run "$HOWDY" rename 1 second
expect "rename back" "$output" "Renamed model 1"
run "$HOWDY" rename 1 ","
expect "rename to nothing but a comma, exit code" "$code" 1
expect_contains "rename to nothing but a comma" "$output" "The new name of the model is empty"

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
run bash -c "echo y | '$HOWDY' remove 0"
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
printf '[core]\n# disabled = false\ndisabled=false\n' > "$T/etc/config.ini"
run as_root "$HOWDY" disable 1
expect "disable, only a comment says it that way: exit code" "$code" 1
expect "disable, the comment is not changed" "$(grep -c '^# disabled = false$' "$T/etc/config.ini")" 1
printf '[core]\n# disabled = false\ndisabled = false\n' > "$T/etc/config.ini"
run as_root "$HOWDY" disable 1
expect "disable changes the option, not the comment" "$(tr '\n' '|' < "$T/etc/config.ini")" "[core]|# disabled = false|disabled = true|"
cp "$CONFIG_TEMPLATE" "$T/etc/config.ini"
chmod 640 "$T/etc/config.ini"
run as_root "$HOWDY" disable maybe
expect "disable, invalid argument" "$output" "Please only use 0 (enable) or 1 (disable) as an argument"
expect "config permissions are kept" "$(stat -c %a "$T/etc/config.ini")" 640
run as_root "$HOWDY" set certainty 80
expect "set" "$output" "Config option updated"
expect_contains "config has the new value" "$(grep '^certainty' "$T/etc/config.ini")" "certainty = 80"
expect "set keeps the other lines" "$(diff <(sed 's/^certainty = .*//;s/^disabled = .*//' "$CONFIG_TEMPLATE") <(sed 's/^certainty = .*//;s/^disabled = .*//' "$T/etc/config.ini") && echo same)" same
run as_root "$HOWDY" set nosuchoption 1
expect "set, unknown option" "$output" 'Could not find a "nosuchoption" config option to set'
# The values of an option in each section, as "section value"
values() { awk -v option="$1" '/^\[/ { s = $0 } $1 == option { print s " " $3 }' "$T/etc/config.ini" | tr '\n' ' '; }
before=$(values max_tries)
run as_root "$HOWDY" set max_tries 7
expect_contains "set, an option of several sections is refused" "$output" "howdy set fingerprint.max_tries 7"
expect "set, nothing is changed by it" "$(values max_tries)" "$before"
run as_root "$HOWDY" set fingerprint.max_tries 7
expect "set with the section" "$output" "Config option updated"
expect "set with the section changes that one only" "$(values max_tries)" "${before/\[fingerprint\] 1/[fingerprint] 7}"
run as_root "$HOWDY" set nosection.max_tries 7
expect "set, unknown section" "$output" 'Could not find a "nosection.max_tries" config option to set'
run as_root "$HOWDY" set certainty
expect_contains "set without a value" "$output" "Please add a setting you would like to change and the value to set it to"

# A value on several lines is replaced as a whole, also behind an empty line and a comment
cp "$CONFIG_TEMPLATE" "$T/etc/config.ini"
chmod 640 "$T/etc/config.ini"
printf '[rubberstamps]\nstamp_rules =\n\tnod 5s failsafe\n\n# a comment\n\tnod 6s failsafe\n\n[debug]\nend_report = false\n' > "$T/etc/config.ini"
run as_root "$HOWDY" set stamp_rules "hotkey 10s faildeadly"
expect "set replaces all lines of a value" "$(tr '\n' '|' < "$T/etc/config.ini")" \
	"[rubberstamps]|stamp_rules = hotkey 10s faildeadly||# a comment||[debug]|end_report = false|"
cp "$CONFIG_TEMPLATE" "$T/etc/config.ini"
chmod 640 "$T/etc/config.ini"

# The editor and systemctl are stand-ins, the real service must not be restarted
mkdir -p "$T/bin"
printf '#!/bin/sh\necho "$@" >> "%s/systemctl.log"\n' "$T" > "$T/bin/systemctl"
printf '#!/bin/sh\necho "# edited" >> "$1"\n' > "$T/bin/fake-editor"
chmod +x "$T/bin/systemctl" "$T/bin/fake-editor"
: > "$T/systemctl.log"
run env EDITOR="$T/bin/fake-editor" PATH="$T/bin:$PATH" unshare -r "$HOWDY" config
expect "config opens the editor" "$output" "Opening config.ini in fake-editor"
expect "config was edited" "$(tail -1 "$T/etc/config.ini")" "# edited"
expect "config leaves the services alone, they read it themselves" "$(cat "$T/systemctl.log")" ""

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
	run xvfb-run -a "$TEST_DIR/dialog_check.sh" escape "$T/enroll_test"
	expect_contains "Escape cancels" "$output" "cancelled"
	expect_contains "and nothing breaks" "$output" "exit code: 1"
	expect_missing "no destroyed widget is used" "$output" "Gtk-CRITICAL"
	# The capture is done after some seconds and shown for a minute: closing it then keeps the samples
	started=$SECONDS
	run xvfb-run -a "$TEST_DIR/dialog_check.sh" late-cancel 20 "$T/enroll_test" --keep-open
	expect_contains "Cancel after the capture keeps the samples" "$output" "samples: "
	expect_contains "and ends well" "$output" "exit code: 0"
	expect "without the rest of the wait" "$([ $((SECONDS - started)) -le 45 ] && echo yes || echo "$((SECONDS - started)) s")" yes

	write_config "$T/blank.mp4"
	run xvfb-run -a "$T/enroll_test"
	expect_contains "no face" "$output" "error: No face detected, aborting"
	write_config /dev/does-not-exist
	run xvfb-run -a "$T/enroll_test"
	expect_contains "enrollment without camera" "$output" "error: Howdy could not find a camera device"
	write_config "$T/still.mp4"

	echo "--- cli, test window"
	# Only root compares faces: without root the command says so before it touches the display or the camera.
	# The window itself is tested as root in compare_as_root.sh.
	write_config /dev/does-not-exist
	run "$HOWDY" test
	expect_contains "test needs root" "$output" "Please run this command as root:"
	expect_contains "and says how" "$output" "sudo howdy test"
	expect_missing "and does not look for the camera" "$output" "camera device"
	write_config "$T/still.mp4"
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
	# A call around the moment it gives up its name is answered, by it or by the next one
	sleep 29.9
	answers=""
	for _ in 1 2 3 4 5 6; do
		answers+="$(mcall List s "$USER_NAME");"
		sleep 0.05
	done
	expect "calls while the daemon stops are answered" "$answers" "$(printf 's "[]";%.0s' 1 2 3 4 5 6)"
fi

second_pass

echo
[ "$failed" = 0 ] && echo "All tests passed${second_pass_skipped:+, but the second pass (as root) was skipped}" || echo "Some tests FAILED, logs in $T"
exit $failed
