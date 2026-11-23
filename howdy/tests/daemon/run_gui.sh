#!/bin/bash
# Test of the graphical interface (howdy gui, also started as howdy-gtk) without
# a person: gui_test.cc clicks the buttons in a virtual display (xvfb-run). The
# management daemon is a stand-in on a private D-Bus that logs which connection
# each call came from, the camera is a video. No root and no camera are needed.
#
#   CAMERA=/dev/video0   also tests the video tab with a real camera. The camera
#                        is opened and a few pictures are shown, nothing is saved.
#
# See common.sh for the other environment variables.
# The data files of the wizard's download slides are looked for where the program
# is configured to look: in a directory of the test, that has links to the real
# files, so the download can be tested without touching them
WORK_DIR="${WORK_DIR:-$(mktemp -d -t howdy-daemon-tests.XXXXXX)}"
REAL_DLIB_DIR="${DLIB_DATA_DIR:-/usr/local/share/howdy/dlib-models}"
REAL_OPENVINO_DIR="${OPENVINO_DATA_DIR:-/usr/local/share/howdy/openvino-models}"
export WORK_DIR
export DLIB_DATA_DIR="$WORK_DIR/test/dlib"
export OPENVINO_DATA_DIR="$WORK_DIR/test/openvino"
mkdir -p "$DLIB_DATA_DIR" "$OPENVINO_DATA_DIR"

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/bus.sh"

link_data_files() {
	ln -sf "$REAL_DLIB_DIR"/*.dat "$DLIB_DATA_DIR/"
	ln -sf "$REAL_OPENVINO_DIR"/*.xml "$REAL_OPENVINO_DIR"/*.bin "$OPENVINO_DATA_DIR/" 2> /dev/null || true
}
link_data_files

for tool in xvfb-run xdotool xprop ffmpeg; do
	command -v "$tool" > /dev/null || { echo "Missing $tool"; exit 1; }
done

link "$T/gui_test" "${BASE_SOURCES[@]}" "${CORE_SOURCES[@]}" "${GUI_CLI_SOURCES[@]}" "${GUI_SOURCES[@]}" "$TEST_DIR/gui_test.cc"
link_cli "$T/howdy"
ln -sf howdy "$T/howdy-gtk"
make_test_videos

export FACE_VIDEO="$T/single.mp4"
export BLANK_VIDEO="$T/blank.mp4"
export FAKE_LOG="$T/calls.log"
export FAKE_MODE_FILE="$T/mode"
export FAKE_MODELS="$T/models.json"
export FAKE_DAEMON_LOG="$T/daemon-calls.log"
export TZ=UTC
USER_NAME="$(id -un)"

# --- Stand-ins: the management daemon, pkexec and a program that would be started

start_bus
start_background "$HOWDY_PYTHON" -I "$TEST_DIR/fake_management.py" "$FAKE_MODELS" "$FAKE_MODE_FILE" "$FAKE_DAEMON_LOG"
wait_for_name org.boltgolt.howdy.management

# The window does everything itself. A program called howdy in the PATH that
# logs its calls shows that no subprocess is started for it.
mkdir -p "$T/bin"
cat > "$T/bin/howdy" <<'EOF'
#!/bin/bash
echo "howdy $*" >> "$FAKE_LOG"
EOF
# What the setup wizard may run as root, the config is not written for real
cat > "$T/bin/config-helper" <<'EOF'
#!/bin/bash
echo "helper $*" >> "$FAKE_LOG"
[ "${FAKE_SET:-}" = "${2:-}" ] && { echo "Please run this command as root" >&2; exit 1; }
exit 0
EOF
cat > "$T/bin/pkexec" <<'EOF'
#!/bin/bash
echo "pkexec $*" >> "$FAKE_LOG"
exec "$@"
EOF
# The download of the data files: gets them from the real ones, or fails
make_install_script() {
	cat > "$1/install.sh" <<EOF
#!/bin/sh
echo "Downloading model 1"
echo "Downloading model 2"
echo "oops" >&2
[ -z "\$FAKE_DOWNLOAD_FAIL" ] || exit 1
$2
EOF
	chmod +x "$1/install.sh"
}
make_install_script "$DLIB_DATA_DIR" "ln -sf '$REAL_DLIB_DIR'/*.dat '$DLIB_DATA_DIR'/"
make_install_script "$OPENVINO_DATA_DIR" "ln -sf '$REAL_OPENVINO_DIR'/*.xml '$REAL_OPENVINO_DIR'/*.bin '$OPENVINO_DATA_DIR'/ 2> /dev/null || true"
chmod +x "$T/bin/howdy" "$T/bin/config-helper" "$T/bin/pkexec"
export PATH="$T/bin:$PATH"

# Cameras for the wizard: a gray video is an infrared camera
ffmpeg -loglevel error -y -f lavfi -i testsrc=s=320x240:d=2 -r 10 -pix_fmt yuv420p "$T/color.mp4"
ffmpeg -loglevel error -y -f lavfi -i testsrc=s=320x240:d=2 -r 10 -vf format=gray -pix_fmt gray "$T/gray.mkv" 2> /dev/null \
	|| ffmpeg -loglevel error -y -f lavfi -i testsrc=s=320x240:d=2 -r 10 -vf format=gray,format=yuv420p "$T/gray.mkv"
mkdir -p "$T/v4l-infrared" "$T/v4l-color" "$T/v4l-empty"
ln -sf "$T/gray.mkv" "$T/v4l-infrared/pci-0000:00:14.0-usb-0:7:1.2-video-index0"
ln -sf "$T/color.mp4" "$T/v4l-color/pci-0000:00:14.0-usb-0:5:1.0-video-index0"

run() {
	: > "$FAKE_LOG"
	output="$(xvfb-run -a timeout 300 "$@" 2>&1 | grep -vE 'Gtk-WARNING|Gdk-WARNING|^$|libEGL|MESA' || true)"
}

# --- The window

echo "--- main window"
rm -f "$FAKE_MODELS" "$FAKE_MODE_FILE"
: > "$FAKE_DAEMON_LOG"
run "$T/gui_test" main-window
calls="$(cat "$FAKE_LOG")"
daemon_calls="$(cat "$FAKE_DAEMON_LOG")"
expect_contains "there is no window before the check" "$output" "window shown before the check: no"
expect_contains "the window appears when it is allowed" "$output" "window shown after the check: yes"
expect_contains "starts without models" "$output" "models: 0"
if grep -qE "^model: 0\|[0-9-]+ [0-9:]+\|Test face$" <<< "$output"; then echo "PASS  a model is added with its name"; else echo "FAIL  a model is added with its name"; failed=1; fi
expect_contains "no name keeps the default one" "$output" "|Model #1"
expect_contains "the model is saved by the window itself" "$daemon_calls" " Add $USER_NAME"
expect_contains "a model is deleted" "$daemon_calls" " Remove $USER_NAME"
expect_missing "the deleted model is gone" "$(sed -n '/step: delete$/,/step: add without a camera/p' <<< "$output")" "Test face"
expect_contains "no camera is reported" "$output" "error: Error while adding model: | Howdy could not find a camera device"
expect_contains "the window is usable after a failure" "$output" "busy after failure: no"
expect_contains "no face is reported" "$output" "error: Error while adding model: | No face detected, aborting"
expect_contains "a model the daemon refuses is reported" "$output" "error: Error while adding model: | The model could not be saved"
expect_contains "a model is renamed" "$daemon_calls" " Rename $USER_NAME"
expect_contains "the new name is shown" "$(sed -n '/step: rename$/,/step: rename fails/p' <<< "$output")" "model: 1|"
expect_contains "the name is the new one" "$(sed -n '/step: rename$/,/step: rename fails/p' <<< "$output")" "|Renamed"
expect_contains "a failed rename is reported" "$output" "error: Error while renaming model | The model could not be renamed"
expect_contains "a rename with an old list is refused" "$output" 'error: Error while renaming model | Model 1 is not called "Another name" any more'
expect_contains "a delete with an old list is refused" "$output" 'error: Error while deleting model | Model 1 is not called "Another name" any more'
expect_contains "the model is kept then" "$(sed -n '/step: the list is out of date$/,/step: the video tab/p' <<< "$output")" "|Renamed"
expect_contains "a failed delete is reported" "$output" "error: Error while deleting model | No model with ID"
expect_contains "the camera of the config is shown" "$output" "camera: video42 | Can't open camera | opencv"
expect_contains "a refusal later is an error and not fatal" "$output" "error: Failed to load the face models | Polkit authorization denied"
expect_contains "the window keeps running after it" "$output" "still running after a refusal: yes"
expect_contains "no permission at the start is fatal" "$output" "fatal: Authorization failed: Polkit authorization denied"
expect_contains "the window never appears without permission" "$output" "denied: window shown: no"
expect_contains "the denied window is not authorized" "$output" "denied: authorized: no"
expect_contains "the window appears when only the list failed" "$output" "failing: window shown: yes"
expect_contains "a failing list is reported" "$output" "error: Failed to load the face models | The model file is invalid"
expect_contains "ends normally" "$output" "finished"

# Nothing is started: the window adds models itself, and for polkit all
# calls come from one process, the password is asked for once and kept
if [ -z "$calls" ]; then echo "PASS  no program was started"; else echo "FAIL  a program was started: $calls"; failed=1; fi
callers="$(cut -d' ' -f1 "$FAKE_DAEMON_LOG" | sort -u | wc -l)"
expect "all calls come from one connection" "$callers" "1"
expect_contains "list, add and remove were all called" "$(cut -d' ' -f2 "$FAKE_DAEMON_LOG" | sort -u | tr '\n' ' ')" "Add List Remove Rename"

# What was saved is a model of the face: the first model was deleted, the
# second one (the default name) is what the daemon has
saved="$("$HOWDY_PYTHON" -I -c "
import json
models = json.load(open('$FAKE_MODELS'))
print(len(models), models[0]['label'], len(models[0]['data']) > 0, {len(e) for e in models[0]['data']})")"
expect "the face is saved as encodings" "$saved" "1 Model #1 True {128}"
rm -f "$FAKE_MODE_FILE"

echo "--- main window, the dialog is closed"
rm -f "$FAKE_MODELS"
run "$TEST_DIR/dialog_check.sh" cancel "$T/gui_test" main-window-add-cancel
expect_contains "closing the dialog ends the add" "$output" "add finished: yes"
expect_contains "no model is added" "$output" "models: 0"
expect_contains "the window is usable afterwards" "$output" "busy afterwards: no"
expect_missing "closing the dialog is not an error" "$output" "error:"

if [ -n "${CAMERA:-}" ]; then
	echo "--- video tab, real camera"
	printf '[video]\ndevice_path = %s\nrecording_plugin = opencv\n' "$CAMERA" > "$T/etc/config.ini"
	run "$T/gui_test" video-tab
	expect_contains "the window is shown" "$output" "window shown: yes"
	expect_contains "a picture arrives in the tab" "$output" "picture arrived: yes"
	expect_missing "no errors" "$output" "error:"
	redraws="$(grep -oE 'picture redraws: [0-9]+' <<< "$output" | grep -oE '[0-9]+$' || echo 0)"
	# 3 seconds of a camera with 15 or more frames per second
	expect "the picture is drawn as it arrives" "$([ "${redraws:-0}" -ge 15 ] && echo drawn || echo "starved ($redraws redraws)")" drawn
fi

# --- The setup wizard

HELPER=(--helper "$T/bin/config-helper")

echo "--- onboarding, infrared camera"
rm -f "$FAKE_MODELS"
: > "$FAKE_DAEMON_LOG"
run "$T/gui_test" onboarding --v4l-dir "$T/v4l-infrared" "${HELPER[@]}"
calls="$(cat "$FAKE_LOG")"
expect_contains "all slides are shown" "$output" "slide: 7"
expect_contains "the emitter is asked about" "$output" "emitter question: yes"
expect_contains "the camera is configured as root" "$calls" "pkexec $T/bin/config-helper set device_path $T/v4l-infrared/pci-0000:00:14.0-usb-0:7:1.2-video-index0"
expect_contains "the model is saved by the wizard itself" "$(cat "$FAKE_DAEMON_LOG")" " Add $USER_NAME"
expect_contains "the balanced certainty is a percentage" "$calls" "pkexec $T/bin/config-helper set certainty 75"
expect_contains "the wizard can be finished" "$output" "finish button shown: yes"
expect_contains "the wizard ends normally" "$output" "finished"
expect_missing "no errors" "$output" "error:"
expect_missing "howdy is not run" "$calls" "howdy "
expect "the first model is number 0" "$("$HOWDY_PYTHON" -I -c "import json; print(json.load(open('$FAKE_MODELS'))[0]['label'])")" "Model #0"

echo "--- onboarding, certainty policies"
run "$T/gui_test" onboarding --v4l-dir "$T/v4l-infrared" "${HELPER[@]}" --certainty fast
expect_contains "fast" "$(cat "$FAKE_LOG")" "set certainty 65"
run "$T/gui_test" onboarding --v4l-dir "$T/v4l-infrared" "${HELPER[@]}" --certainty secure
expect_contains "secure" "$(cat "$FAKE_LOG")" "set certainty 88"

echo "--- onboarding, the dialog of the scan is closed"
run "$TEST_DIR/dialog_check.sh" cancel "$T/gui_test" onboarding --v4l-dir "$T/v4l-infrared" "${HELPER[@]}" --add-cancels
expect_contains "closing the dialog is not an error" "$output" "scan cancelled: stays on slide 5, scan button usable again: yes"
expect_missing "no error" "$output" "error:"

echo "--- onboarding, other cameras"
run "$T/gui_test" onboarding --v4l-dir "$T/v4l-color" "${HELPER[@]}"
expect_contains "no emitter question for a camera that is not infrared" "$output" "emitter question: skipped"
expect_contains "it is still configured" "$(cat "$FAKE_LOG")" "set device_path $T/v4l-color/"
run "$T/gui_test" onboarding --v4l-dir "$T/v4l-infrared" "${HELPER[@]}" --infrared-no
expect_contains "a missing emitter is explained" "$output" "emitter buttons hidden: yes"
expect_contains "and blocks the wizard" "$output" "next is blocked: yes"
run "$T/gui_test" onboarding --v4l-dir "$T/v4l-empty" "${HELPER[@]}"
expect_contains "no camera" "$output" "error: No webcams found on system"
run "$T/gui_test" onboarding --v4l-dir "$T/does-not-exist" "${HELPER[@]}"
expect_contains "no camera directory" "$output" "error: No webcams found on system"

echo "--- onboarding, the data files are downloaded"
rm -f "$DLIB_DATA_DIR"/*.dat "$OPENVINO_DATA_DIR"/*.xml "$OPENVINO_DATA_DIR"/*.bin
run "$T/gui_test" onboarding --v4l-dir "$T/v4l-infrared" "${HELPER[@]}"
calls="$(cat "$FAKE_LOG")"
expect_contains "dlib files are downloaded as root" "$calls" "pkexec /bin/sh -c cd '$DLIB_DATA_DIR' && ./install.sh"
expect_contains "OpenVINO files are downloaded as root" "$calls" "pkexec /bin/sh -c cd '$OPENVINO_DATA_DIR' && ./install.sh"
expect_contains "the downloads end" "$output" "download output: Done!"
expect_contains "the wizard goes on with the downloaded data" "$output" "slide: 7"
expect_missing "no errors" "$output" "error:"
rm -f "$DLIB_DATA_DIR"/*.dat "$OPENVINO_DATA_DIR"/*.xml "$OPENVINO_DATA_DIR"/*.bin
FAKE_DOWNLOAD_FAIL=1 run "$T/gui_test" onboarding --v4l-dir "$T/v4l-infrared" "${HELPER[@]}"
expect_contains "a failed download is reported" "$output" "error: Error while downloading datafiles | Downloading model 1"
link_data_files
run "$T/gui_test" onboarding --v4l-dir "$T/v4l-infrared" "${HELPER[@]}" --certainty fast
expect_contains "existing dlib files are not downloaded again" "$output" "DLib datafiles have already been downloaded!"
expect_contains "existing OpenVINO files are not downloaded again" "$output" "OpenVINO datafiles have already been downloaded!"
expect_missing "no download" "$(cat "$FAKE_LOG")" "install.sh"

echo "--- onboarding, failures"
FAKE_SET=device_path run "$T/gui_test" onboarding --v4l-dir "$T/v4l-infrared" "${HELPER[@]}"
expect_contains "a camera that can't be set is reported" "$output" "error: Error setting camera path"
FAKE_SET=certainty run "$T/gui_test" onboarding --v4l-dir "$T/v4l-infrared" "${HELPER[@]}"
expect_contains "a certainty that can't be set is reported" "$output" "error: Error setting certainty"

# --- The program itself: one program, started in two ways

echo "--- the program"
# Without models the setup wizard starts, otherwise the main window, which asks
# for the models. The window stays open, so it is stopped after a while.
mkdir -p "$T/models"
write_empty_config() { printf '[video]\ndevice_path = /dev/video42\nrecording_plugin = opencv\n' > "$T/etc/config.ini"; }
write_empty_config
: > "$FAKE_DAEMON_LOG"
code=0
xvfb-run -a timeout 4 "$T/howdy" gui > "$T/app.log" 2>&1 || code=$?
expect_contains "howdy gui starts the main window when there are models" "$(cat "$FAKE_DAEMON_LOG")" " List "
expect "it keeps running" "$code" 124
: > "$FAKE_DAEMON_LOG"
code=0
xvfb-run -a timeout 4 "$T/howdy-gtk" > "$T/app.log" 2>&1 || code=$?
expect_contains "howdy-gtk is the same program" "$(cat "$FAKE_DAEMON_LOG")" " List "
expect "it keeps running too" "$code" 124
: > "$FAKE_DAEMON_LOG"
code=0
xvfb-run -a timeout 4 "$T/howdy-gtk" --force-onboarding > "$T/app.log" 2>&1 || code=$?
expect_missing "the wizard is started on request" "$(cat "$FAKE_DAEMON_LOG")" " List "
expect "the wizard keeps running" "$code" 124
# The models directory is not readable for the user, only the daemon knows that there are no models
rmdir "$T/models"
rm -f "$FAKE_MODELS"
: > "$FAKE_DAEMON_LOG"
code=0
xvfb-run -a timeout 4 "$T/howdy" gui > "$T/app.log" 2>&1 || code=$?
expect_contains "the daemon is asked for the models" "$(cat "$FAKE_DAEMON_LOG")" " List "
expect "the wizard is started without models and keeps running" "$code" 124
expect_missing "no errors on start" "$(grep -v 'Gtk-WARNING\|Gdk-WARNING' "$T/app.log" || true)" "rror"
run "$T/howdy" -h
expect_contains "gui is a command" "$output" "disable, gui, list"

echo
[ "$failed" = 0 ] && echo "All tests passed" || echo "Some tests FAILED, logs in $T"
exit $failed
