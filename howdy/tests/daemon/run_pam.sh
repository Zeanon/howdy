#!/bin/bash
# Test of pam_howdy.so with a real PAM stack (pam_start_confdir) against the
# daemon on a private D-Bus: who is asked to look into the camera and what the
# module returns. A video made from a still image is used as camera. The module
# only reads a configuration that belongs to root, so the test runs in a user
# namespace, in which the user is root.
# See common.sh for the environment variables.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

PAM_SRC_DIR="$(cd "$TEST_DIR/../../src/pam" && pwd)"
PAM_DIR="$WORK_DIR/pam"
mkdir -p "$PAM_DIR"

# The module with the paths of the test, built here, the test runs in the namespace
if [ -z "${HOWDY_IN_NAMESPACE:-}" ]; then
	configure "$PAM_SRC_DIR/vars.hh" "$PAM_DIR/vars.hh"
	sed -i "s|@config_file_path@|$T/etc/config.ini|" "$PAM_DIR/vars.hh"
	read -r -a PAM_CFLAGS <<< "$(pkg-config --cflags INIReader libsystemd libevdev | system_includes)"
	read -r -a PAM_LIBS <<< "$(pkg-config --libs INIReader libsystemd libevdev)"
	"$CXX" -std=c++20 -fPIC -shared -DGETTEXT_PACKAGE='"howdy"' -DLOCALEDIR='"/nonexistent"' -I"$PAM_DIR" -I"$PAM_SRC_DIR" "${PAM_CFLAGS[@]}" \
		"$PAM_SRC_DIR"/{main,face_authenticator,fingerprint_authenticator,enter_device}.cc -o "$PAM_DIR/pam_howdy.so" "${PAM_LIBS[@]}" -lpam -lpthread
	"$CXX" -std=c++20 "$TEST_DIR/pam_test.cc" -o "$PAM_DIR/pam_test" -lpam
	link_daemon "$T/daemon"

	command -v unshare > /dev/null || { echo "Missing unshare"; exit 1; }
	REAL_USER="$(id -un)" HOWDY_IN_NAMESPACE=1 WORK_DIR="$WORK_DIR" exec unshare -Ur "$BASH" "${BASH_SOURCE[0]}"
fi

source "$(dirname "${BASH_SOURCE[0]}")/bus.sh"
start_bus

for tool in ffmpeg; do
	command -v "$tool" > /dev/null || { echo "Missing $tool"; exit 1; }
done

ffmpeg -loglevel error -y -loop 1 -i "$IMG_DIR/2008_001009.jpg" -t 3 -r 15 -pix_fmt yuv420p -vf scale=360:480 "$T/still.mp4"
"$HOWDY_PYTHON" -I "$TEST_DIR/parity.py" "$WORK_DIR/pyhowdy" hog "$IMG_DIR/2008_001009.jpg" "$IMG_DIR/2008_002079.jpg" > "$T/hog.json"
"$HOWDY_PYTHON" -I "$TEST_DIR/mkmodel.py" "$T/hog.json" 2008_001009.jpg 0 "$T/genuine.dat"
"$HOWDY_PYTHON" -I "$TEST_DIR/mkmodel.py" "$T/hog.json" 2008_002079.jpg 0 "$T/impostor.dat"

# The user whose face is checked, not root of the namespace
USER_NAME="$REAL_USER"
MODEL="$T/models/$USER_NAME.dat"
"$HOWDY_PYTHON" -I "$TEST_DIR/mkconf.py" "$CONFIG_TEMPLATE" "$T/etc/config.ini" \
	"video.device_path=$T/still.mp4" video.recording_plugin=ffmpeg video.device_format=mov face_authentication.timeout=3 \
	face_authentication.micromotion_detection=false face_authentication.texture_detection=false face_authentication.similarity_detection=false
chmod 644 "$T/etc/config.ini"
start_background "$T/daemon"
wait_for_name org.boltgolt.howdy.authentication

CONFDIR="$PAM_DIR/pam.d"
mkdir -p "$CONFDIR"
echo "auth required $PAM_DIR/pam_howdy.so max-tries=1" > "$CONFDIR/howdy-test"

# Runs the PAM stack, sets OUT (messages and result) and SECONDS_TAKEN
run_pam() {
	local start=${EPOCHREALTIME/[.,]/}
	OUT=$("$PAM_DIR/pam_test" "$CONFDIR" howdy-test "$USER_NAME" 2>&1)
	SECONDS_TAKEN=$(( (${EPOCHREALTIME/[.,]/} - start) / 1000000 ))
}
result_of() { grep '^RESULT' <<< "$OUT" | cut -d' ' -f2; }
has() { grep -qF "$1" <<< "$OUT" && echo yes || echo no; }

echo "--- no face model"
rm -f "$MODEL"
run_pam
expect "the password is asked for, the face does not authenticate" "$([ "$(result_of)" != 0 ] && echo yes || echo no)" yes
expect "returns at once" "$([ "$SECONDS_TAKEN" -lt 2 ] && echo yes || echo no)" yes
expect "nobody is asked to look into the camera" "$(has "Please look directly into the camera")" no
expect "nothing is announced" "$(has "Facial authentication")" no
expect "there is no error message for the user" "$(has "MSG 3")" no

echo "--- genuine face"
mkdir -p "$T/models"
cp "$T/genuine.dat" "$MODEL"
chmod 600 "$MODEL"
run_pam
expect "authenticated" "$(result_of)" 0
expect "the user is asked to look into the camera" "$(has "Please look directly into the camera")" yes
expect "the authentication is announced" "$(has "Facial authentication")" yes
expect "the user is told who authenticated" "$(has "Face authenticated as $USER_NAME")" yes
expect "the messages come in the order they always had" "$(grep '^MSG' <<< "$OUT" | head -3 | tr '\n' '|')" "MSG 4: Facial authentication...|MSG 4: Please look directly into the camera|MSG 4: |"

echo "--- foreign face"
cp "$T/impostor.dat" "$MODEL"
chmod 600 "$MODEL"
run_pam
expect "not authenticated" "$([ "$(result_of)" != 0 ] && echo yes || echo no)" yes
expect "the user was asked to look into the camera" "$(has "Please look directly into the camera")" yes

exit $failed
