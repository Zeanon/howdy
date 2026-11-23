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
	# The module that checks the password, from the first directory of the distributions that has it
	PAM_UNIX=""
	for dir in /usr/lib/x86_64-linux-gnu/security /usr/lib64/security /usr/lib/aarch64-linux-gnu/security /usr/lib/security /lib/security; do
		[ -f "$dir/pam_unix.so" ] && { PAM_UNIX="$dir/pam_unix.so"; break; }
	done
	sed -i "s|@pam_unix_path@|${PAM_UNIX:-/nonexistent/pam_unix.so}|" "$PAM_DIR/vars.hh"
	read -r -a PAM_CFLAGS <<< "$(pkg-config --cflags INIReader libsystemd libevdev | system_includes)"
	read -r -a PAM_LIBS <<< "$(pkg-config --libs INIReader libsystemd libevdev)"
	"$CXX" -std=c++20 -fPIC -shared -DGETTEXT_PACKAGE='"howdy"' -DLOCALEDIR='"/nonexistent"' -I"$PAM_DIR" -I"$PAM_SRC_DIR" "${PAM_CFLAGS[@]}" \
		"$PAM_SRC_DIR"/{main,face_authenticator,fingerprint_authenticator,enter_device}.cc -o "$PAM_DIR/pam_howdy.so" "${PAM_LIBS[@]}" -lpam -lpthread
	"$CXX" -std=c++20 "$TEST_DIR/pam_test.cc" -o "$PAM_DIR/pam_test" -lpam
	"$CXX" -shared -fPIC -x c "$TEST_DIR/fake_logind.c" -o "$PAM_DIR/fake_logind.so"
	link_daemon "$T/daemon"

	command -v unshare > /dev/null || { echo "Missing unshare"; exit 1; }
	REAL_USER="$(id -un)" HOWDY_IN_NAMESPACE=1 WORK_DIR="$WORK_DIR" exec unshare -Ur "$BASH" "${BASH_SOURCE[0]}"
fi

# The test does not depend on where it is started from, over SSH or not
unset SSH_CONNECTION SSH_CLIENT SSH_TTY SSHD_OPTS

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
DAEMON_PID=$BACKGROUND_PID
wait_for_name org.boltgolt.howdy.authentication

CONFDIR="$PAM_DIR/pam.d"
mkdir -p "$CONFDIR"
echo "auth required $PAM_DIR/pam_howdy.so max-tries=1" > "$CONFDIR/howdy-test"

# Runs the PAM stack, sets OUT (messages and result) and SECONDS_TAKEN
run_pam() {
	local start=${EPOCHREALTIME/[.,]/}
	# fake_logind.so makes the login session a local one, or a remote one with HOWDY_TEST_REMOTE=1
	OUT=$(LD_PRELOAD="$PAM_DIR/fake_logind.so" "$PAM_DIR/pam_test" "$CONFDIR" howdy-test "$USER_NAME" 2>&1)
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

echo "--- the daemon is restarted"
# The daemon is restarted when the computer wakes up and is gone for a few seconds. The module waits
# for it and asks again, it does not fail: a failure would be a failed attempt for the lock screen.
stop_daemon() {
	kill "$DAEMON_PID" 2> /dev/null || true
	while kill -0 "$DAEMON_PID" 2> /dev/null; do sleep 0.1; done
}
run_pam_in_background() {
	( LD_PRELOAD="$PAM_DIR/fake_logind.so" "$PAM_DIR/pam_test" "$CONFDIR" howdy-test "$USER_NAME" > "$T/pam_background.out" 2>&1 ) &
	pam_pid=$!
}

# Gone when the module starts, back after two seconds
cp "$T/genuine.dat" "$MODEL"
chmod 600 "$MODEL"
stop_daemon
started=$SECONDS
run_pam_in_background
sleep 2
start_background "$T/daemon"
DAEMON_PID=$BACKGROUND_PID
wait "$pam_pid"
OUT=$(cat "$T/pam_background.out")
expect "a daemon that is not there at first: the face authenticates" "$(result_of)" 0
expect "after it came back" "$([ $((SECONDS - started)) -ge 2 ] && echo yes || echo no)" yes

# Restarted during the request: the first request is lost, the second one is made at the new daemon.
# The face of the first one does not match, the model is changed while the daemon is gone.
cp "$T/impostor.dat" "$MODEL"
chmod 600 "$MODEL"
started=$SECONDS
run_pam_in_background
sleep 1
stop_daemon
cp "$T/genuine.dat" "$MODEL"
chmod 600 "$MODEL"
sleep 1
start_background "$T/daemon"
DAEMON_PID=$BACKGROUND_PID
wait "$pam_pid"
OUT=$(cat "$T/pam_background.out")
expect "a daemon that is restarted during the request: the face authenticates" "$(result_of)" 0
expect "and it took the time of the restart" "$([ $((SECONDS - started)) -ge 2 ] && echo yes || echo no)" yes
cp "$T/impostor.dat" "$MODEL"

echo "--- the methods that can be used are announced"
# fprintd on the private bus, its mode is read at every call
export FPRINTD_MODE_FILE="$T/fprintd.mode"
echo wait > "$FPRINTD_MODE_FILE"
start_background "$HOWDY_PYTHON" -I "$TEST_DIR/fake_fprintd.py"
wait_for_name net.reactivated.Fprint
echo "auth required $PAM_DIR/pam_howdy.so max-tries=1 init-message" > "$CONFDIR/howdy-test"
"$HOWDY_PYTHON" -I "$TEST_DIR/mkconf.py" "$T/etc/config.ini" "$T/etc/config.ini" fingerprint.enable=true fingerprint.timeout=6s
chmod 644 "$T/etc/config.ini"

position() { grep -nF "$1" <<< "$OUT" | head -1 | cut -d: -f1; }
INIT_FACE="Look directly into the camera"
INIT_FINGER="Place your finger on the fingerprint reader"

# scenario <name> <fprintd mode> <face model: yes|no> sets OUT
scenario() {
	echo "$2" > "$FPRINTD_MODE_FILE"
	if [ "$3" = yes ]; then cp "$T/genuine.dat" "$MODEL"; chmod 600 "$MODEL"; else rm -f "$MODEL"; fi
	run_pam
	[ -z "${SHOW:-}" ] || echo "$OUT" | sed "s/^/    | /"
}

scenario "both" wait yes
expect "face and fingerprint: authenticated by the face" "$(result_of)" 0
expect "face and fingerprint: both are offered" "$(has "$INIT_FINGER")$(has "Or look directly into the camera")" yesyes
expect "face and fingerprint: the face is announced" "$(has "Facial authentication...")" yes
expect "face and fingerprint: the fingerprint is announced" "$(has "Fingerprint authentication...")" yes
expect "face and fingerprint: the order of the messages" "$([ "$(position "$INIT_FINGER")" -lt "$(position "Facial authentication...")" ] && [ "$(position "Please look directly into the camera")" -lt "$(position "Fingerprint authentication...")" ] && echo ordered)" ordered
expect "face and fingerprint: the messages are in the order of the console" "$(grep -v '^RESULT' <<< "$OUT" | sed 's/^MSG [0-9]*: //' | head -7 | tr '\n' '|')" "Place your finger on the fingerprint reader|Or look directly into the camera|Facial authentication...|Please look directly into the camera||Fingerprint authentication...|Place your finger on the fingerprint reader|"

# A slow reader holds back only its own notice, the face is announced at once
# (without init-message, which waits for both methods)
echo "auth required $PAM_DIR/pam_howdy.so max-tries=1" > "$CONFDIR/howdy-test"
export PAM_TEST_TIMES="$T/pam_times"
rm -f "$PAM_TEST_TIMES"
scenario "slow reader" slow yes
unset PAM_TEST_TIMES
echo "auth required $PAM_DIR/pam_howdy.so max-tries=1 init-message" > "$CONFDIR/howdy-test"
at() { awk -F'\t' -v text="$1" '$2 == text { print $1; exit }' "$T/pam_times"; }
expect "a slow reader: authenticated by the face" "$(result_of)" 0
expect "a slow reader: the face is announced at once" "$([ "$(at "Facial authentication...")" -lt 300 ] && echo yes)" yes
expect "a slow reader: the fingerprint is announced when its reader answers" "$([ "$(at "Fingerprint authentication...")" -ge 500 ] && echo yes)" yes
expect "a slow reader: the messages are in the usual order" "$(grep -v '^RESULT' <<< "$OUT" | sed 's/^MSG [0-9]*: //' | head -5 | tr '\n' '|')" "Facial authentication...|Please look directly into the camera||Fingerprint authentication...|Place your finger on the fingerprint reader|"

scenario "no fingerprints" no-prints yes
expect "no fingerprints: authenticated by the face" "$(result_of)" 0
expect "no fingerprints: only the face is offered" "$(has "$INIT_FACE")$(has "$INIT_FINGER")" yesno
expect "no fingerprints: the fingerprint is not announced" "$(has "Fingerprint authentication...")" no
expect "no fingerprints: the face is announced" "$(has "Facial authentication...")" yes

scenario "no face model" match no
expect "no face model: authenticated by the finger" "$(result_of)" 0
expect "no face model: only the fingerprint is offered" "$(has "$INIT_FINGER")$(has "$INIT_FACE")" yesno
expect "no face model: the face is not announced" "$(has "Facial authentication...")$(has "Please look directly into the camera")" nono
expect "no face model: the fingerprint is announced" "$(has "Fingerprint authentication...")" yes
expect "no face model: the user is told who authenticated" "$(has "Fingerprint authenticated as $USER_NAME")" yes

scenario "no reader" no-device no
expect "nothing to use: not authenticated" "$([ "$(result_of)" != 0 ] && echo yes || echo no)" yes
expect "nothing to use: nothing is said to the user" "$(grep -c '^MSG' <<< "$OUT" || true)" 0

scenario "no fingerprints, no face model" no-prints no
expect "nothing to use with enrolled fingerprints missing: nothing is said" "$(grep -c '^MSG' <<< "$OUT" || true)" 0
expect "and it does not take long" "$([ "$SECONDS_TAKEN" -lt 3 ] && echo yes || echo no)" yes

echo "--- a remote session is skipped"
# The environment can be changed by the user, the login session of logind can not
echo no-prints > "$FPRINTD_MODE_FILE"
cp "$T/genuine.dat" "$MODEL"
chmod 600 "$MODEL"
echo "auth required $PAM_DIR/pam_howdy.so max-tries=1" > "$CONFDIR/howdy-test"
run_pam
expect "a local session: the face authenticates" "$(result_of)" 0
SSH_CONNECTION="192.0.2.1 5000 192.0.2.2 22" run_pam
# The module returns PAM_IGNORE, which libpam makes PAM_PERM_DENIED (6) if it is the only module. A face that does not match would be 7.
expect "ssh in the environment: skipped" "$(result_of)" 6
expect "ssh in the environment: nothing is said" "$(grep -c '^MSG' <<< "$OUT" || true)" 0
HOWDY_TEST_REMOTE=1 run_pam
expect "remote session of logind, the environment is clean: skipped" "$(result_of)" 6
expect "remote session of logind: nothing is said" "$(grep -c '^MSG' <<< "$OUT" || true)" 0
echo "auth required $PAM_DIR/pam_howdy.so max-tries=1 abort-if-ssh=false" > "$CONFDIR/howdy-test"
HOWDY_TEST_REMOTE=1 run_pam
expect "abort-if-ssh=false: the remote session is not skipped" "$(result_of)" 0
echo "auth required $PAM_DIR/pam_howdy.so max-tries=1" > "$CONFDIR/howdy-test"

echo "--- the password prompt comes first"
# The prompt is printed by the password task, the notices by the module. Without a wait between them the
# order depends on which is faster, so it is tried more than once.
echo wait > "$FPRINTD_MODE_FILE"
echo "auth required $PAM_DIR/pam_howdy.so max-tries=1 password password:nodelay" > "$CONFDIR/howdy-test"
cp "$T/genuine.dat" "$MODEL"
chmod 600 "$MODEL"
prompt_first=0
runs=10
for _ in $(seq "$runs"); do
	run_pam
	[ -z "${SHOW:-}" ] || echo "$OUT" | sed "s/^/    | /"
	[ -n "$(position "MSG 1:")" ] && [ -n "$(position "Facial authentication...")" ] && [ "$(position "MSG 1:")" -lt "$(position "Facial authentication...")" ] && prompt_first=$((prompt_first + 1))
done
expect "the password prompt comes before the notices in every run" "$prompt_first" "$runs"

echo "--- the password prompt comes first, with the password only"
# Nothing to wait for and nothing to set apart from the prompt here, so there are no blank lines
"$HOWDY_PYTHON" -I "$TEST_DIR/mkconf.py" "$T/etc/config.ini" "$T/etc/config.ini" face_authentication.enable=false fingerprint.enable=false
chmod 644 "$T/etc/config.ini"
prompt_first=0
for _ in $(seq "$runs"); do
	run_pam
	[ -z "${SHOW:-}" ] || echo "$OUT" | sed "s/^/    | /"
	[ -n "$(position "MSG 1:")" ] && { [ -z "$(position "MSG 4:")" ] || [ "$(position "MSG 1:")" -lt "$(position "MSG 4:")" ]; } && prompt_first=$((prompt_first + 1))
done
expect "the password prompt is there in every run, before any blank line" "$prompt_first" "$runs"
expect "there are no blank lines, nothing follows them" "$(has "MSG 4")" no

exit $failed
