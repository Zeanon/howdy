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
	"$CXX" -std=c++20 -I"$PAM_DIR" -I"$PAM_SRC_DIR" "${PAM_CFLAGS[@]}" -DGETTEXT_PACKAGE='"howdy"' -DLOCALEDIR='"/nonexistent"' \
		"$TEST_DIR/face_cancel_test.cc" "$PAM_SRC_DIR/face_authenticator.cc" -o "$PAM_DIR/face_cancel_test" "${PAM_LIBS[@]}" -lpthread
	"$CXX" -std=c++20 -I"$PAM_DIR" -I"$PAM_SRC_DIR" "${PAM_CFLAGS[@]}" "$TEST_DIR/timeout_test.cc" -o "$PAM_DIR/timeout_test" "${PAM_LIBS[@]}"
	"$CXX" -shared -fPIC -x c "$TEST_DIR/fake_logind.c" -o "$PAM_DIR/fake_logind.so"
	link_daemon "$T/daemon"

	command -v unshare > /dev/null || { echo "Missing unshare"; exit 1; }
	REAL_USER="$(id -un)" HOWDY_IN_NAMESPACE=1 WORK_DIR="$WORK_DIR" exec unshare -Ur "$BASH" "${BASH_SOURCE[0]}"
fi

# The test does not depend on where it is started from, over SSH or not
unset SSH_CONNECTION SSH_CLIENT SSH_TTY SSHD_OPTS

source "$(dirname "${BASH_SOURCE[0]}")/bus.sh"
start_bus

for tool in ffmpeg nm; do
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

echo "--- the log of the program"
# openlog() and closelog() would change the log of the whole program that uses PAM, the module does not call them
# (pam_get_user shows that nm has read the module, without it no openlog would be found either)
symbols=$(nm -D --undefined-only "$PAM_DIR/pam_howdy.so" || true)
expect "the symbols of the module are read" "$(grep -qE ' pam_get_user(@|$)' <<< "$symbols" && echo yes || echo no)" yes
expect "the module leaves the log name of the program alone" "$(grep -cE ' (openlog|closelog)(@|$)' <<< "$symbols" || true)" 0

# What the module returns (pam_test prints the number): 7 PAM_AUTH_ERR, 9 PAM_AUTHINFO_UNAVAIL, 11 PAM_MAXTRIES, 19 PAM_CONV_ERR
echo "--- how the timeouts are read"
# (a "!" marks a value the module warns about in the log, it is still read as before)
expect "documented timeouts are read without a warning" "$("$PAM_DIR/timeout_test" -1 30 30s 5m 30min 2h 1day)" "-1=4294967295 30=30 30s=30 5m=300 30min=1800 2h=7200 1day=86400 "
expect "many days do not wrap around to a short time" "$("$PAM_DIR/timeout_test" 178956971d)" "178956971d=15461882294400 "
expect "other values are warned about" "$("$PAM_DIR/timeout_test" -30 5x 5mx -1m abc 99999999999 | sed 's/=[0-9]*//g')" "-30! 5x! 5mx! -1m! abc! 99999999999! "
expect "a delay is not negative and at most a minute" "$("$PAM_DIR/timeout_test" --delay 0 2s 1m -1 -5s 5m 2x abc 99999999999)" "0=0 2s=2 1m=60 -1=2! -5s=2! 5m=60! 2x=2! abc=2! 99999999999=2! "

echo "--- no face model"
rm -f "$MODEL"
run_pam
expect "the password is asked for, the face does not authenticate" "$([ "$(result_of)" != 0 ] && echo yes || echo no)" yes
expect "without a face model the face is not available" "$(result_of)" 9
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

# A huge timeout ("-1m" is billions of minutes) is a long one, not one that has already passed
echo "auth required $PAM_DIR/pam_howdy.so max-tries=1 global-timeout=-1m" > "$CONFDIR/howdy-test"
run_pam
expect "a huge global timeout: authenticated" "$(result_of)" 0
echo "auth required $PAM_DIR/pam_howdy.so max-tries=1" > "$CONFDIR/howdy-test"

echo "--- foreign face"
cp "$T/impostor.dat" "$MODEL"
chmod 600 "$MODEL"
run_pam
expect "not authenticated" "$([ "$(result_of)" != 0 ] && echo yes || echo no)" yes
expect "a face that does not match: no tries left" "$(result_of)" 11
expect "the user was asked to look into the camera" "$(has "Please look directly into the camera")" yes

echo "--- the authentication is ended during the scan"
# A cancelled face says so, and not that it is not available (the lock screen would not ask it again)
CANCEL_OUT=$("$PAM_DIR/face_cancel_test" "$USER_NAME" 1000 2>&1)
expect "cancel() during the scan: the result is cancelled" "$(awk '{print $2}' <<< "$CANCEL_OUT")" cancelled
echo "auth required $PAM_DIR/pam_howdy.so max-tries=1 global-timeout=1" > "$CONFDIR/howdy-test"
sleep 2
run_pam
# (the face is ended by force here, the rule for a method that still says that it was cancelled is checked
# with the fingerprint below)
expect "the global timeout: a failed attempt" "$(result_of)" 7
sleep 2

echo "--- a wrong password and a face that does not match"
echo "auth required $PAM_DIR/pam_howdy.so max-tries=1 password password:nodelay" > "$CONFDIR/howdy-test"
run_pam
expect "the result of the face, the first method with a failed attempt" "$(result_of)" 11
echo "auth required $PAM_DIR/pam_howdy.so max-tries=1" > "$CONFDIR/howdy-test"

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

echo "--- the daemon starts late"
# A daemon that is still starting (after the computer woke up) does not say yet whether the user has a face
# model. The method that has not answered after a second is not announced, a user without a model would
# be told to look into the camera. The notices of the face come when the daemon answers.
echo "auth required $PAM_DIR/pam_howdy.so max-tries=1 init-message" > "$CONFDIR/howdy-test"
late_daemon() {
	stop_daemon
	run_pam_in_background
	sleep 1.6
	start_background "$T/daemon"
	DAEMON_PID=$BACKGROUND_PID
	wait "$pam_pid"
	OUT=$(cat "$T/pam_background.out")
}
rm -f "$MODEL"
late_daemon
expect "a late daemon, no face model: nothing is announced" "$(has "Look directly into the camera")$(has "Facial authentication...")" nono
expect "a late daemon, no face model: not authenticated" "$([ "$(result_of)" != 0 ] && echo yes || echo no)" yes
cp "$T/genuine.dat" "$MODEL"
chmod 600 "$MODEL"
late_daemon
expect "a late daemon: the face authenticates" "$(result_of)" 0
expect "a late daemon: the first notice is not given, the face has not answered" "$(has "Look directly into the camera")" no
expect "a late daemon: the notices come when the face answers" "$(has "Facial authentication...")$(has "Please look directly into the camera")" yesyes
echo "auth required $PAM_DIR/pam_howdy.so max-tries=1" > "$CONFDIR/howdy-test"
cp "$T/impostor.dat" "$MODEL"

echo "--- the daemon is busy"
# Another authentication of the user runs (the face of the first one does not match, it ends after the
# timeout of the face): the daemon says Busy to the second one, and it is asked again until the first one is over
cp "$T/impostor.dat" "$MODEL"
chmod 600 "$MODEL"
run_pam_in_background
first_pid=$pam_pid
sleep 1
cp "$T/genuine.dat" "$MODEL"
chmod 600 "$MODEL"
run_pam
wait "$first_pid" || true
expect "a daemon that is busy: the face authenticates when it is free" "$(result_of)" 0
expect "a daemon that is busy: it waited for the first request" "$([ "$SECONDS_TAKEN" -ge 1 ] && echo yes || echo no)" yes
cp "$T/impostor.dat" "$MODEL"

echo "--- a daemon that never comes"
# The module waits 20 seconds for a daemon that is not there. Then the face has failed like without a
# daemon: the program is not ended (a signal would end sudo or the lock screen), the others go on.
echo "auth required $PAM_DIR/pam_howdy.so max-tries=1" > "$CONFDIR/howdy-test"
cp "$T/genuine.dat" "$MODEL"
chmod 600 "$MODEL"
stop_daemon
started=$SECONDS
run_pam_in_background
status=0
wait "$pam_pid" || status=$?
OUT=$(cat "$T/pam_background.out")
expect "no daemon: the face does not authenticate" "$([ "$(result_of)" != 0 ] && echo yes || echo no)" yes
expect "no daemon: an error, not 'not available' (the lock screen asks again)" "$(result_of)" 19
expect "no daemon: the program ends normally, not by a signal" "$status" 0
expect "no daemon: the module waited for the daemon" "$([ $((SECONDS - started)) -ge 19 ] && echo yes || echo no)" yes

# The global timeout is counted from the start, also while the module waits to announce a method that has
# not answered
echo "auth required $PAM_DIR/pam_howdy.so max-tries=1 global-timeout=3" > "$CONFDIR/howdy-test"
run_pam
expect "no daemon, a global timeout: a failed attempt" "$(result_of)" 7
expect "no daemon, a global timeout: it ends the wait for the notices" "$([ "$SECONDS_TAKEN" -lt 8 ] && echo yes || echo no)" yes
echo "auth required $PAM_DIR/pam_howdy.so max-tries=1" > "$CONFDIR/howdy-test"
# The same from the config file, with the documented name and with the one that was read before
cp "$T/etc/config.ini" "$T/config.ini.saved"
for key in global_timeout timeout; do
	"$HOWDY_PYTHON" -I "$TEST_DIR/mkconf.py" "$T/config.ini.saved" "$T/etc/config.ini" "core.$key=3"
	chmod 644 "$T/etc/config.ini"
	run_pam
	expect "$key in the config file: a failed attempt after it" "$(result_of) $([ "$SECONDS_TAKEN" -lt 8 ] && echo in-time)" "7 in-time"
done
cp "$T/config.ini.saved" "$T/etc/config.ini"
start_background "$T/daemon"
DAEMON_PID=$BACKGROUND_PID
cp "$T/impostor.dat" "$MODEL"

echo "--- a signal that ends the authentication goes on to the program"
# The module catches SIGINT, SIGTERM and SIGQUIT while it authenticates, so that a Ctrl+C ends the
# authentication, and gives the same signal back to the program when it is done. A program that does
# not handle it is ended by it, like without the module (and not by another signal).
stop_daemon
for signal in INT TERM QUIT; do
	# (a background job of a shell ignores SIGINT and SIGQUIT, the program must not)
	( ulimit -c 0; exec env --default-signal=INT,QUIT LD_PRELOAD="$PAM_DIR/fake_logind.so" "$PAM_DIR/pam_test" "$CONFDIR" howdy-test "$USER_NAME" > "$T/pam_signal.out" 2>&1 ) &
	signal_pid=$!
	# The module waits for the daemon, which is not there
	sleep 1
	kill "-$signal" "$signal_pid"
	status=0
	# (the shell reports a program that is ended by SIGQUIT)
	{ wait "$signal_pid" || status=$?; } 2> /dev/null
	expected="$((128 + $(kill -l "$signal")))"
	expect "SIG$signal ends the program with SIG$signal" "$status" "$expected"
done
start_background "$T/daemon"
DAEMON_PID=$BACKGROUND_PID

echo "--- the methods that can be used are announced"
# fprintd on the private bus, its mode is read at every call
export FPRINTD_MODE_FILE="$T/fprintd.mode"
echo wait > "$FPRINTD_MODE_FILE"
export FPRINTD_LOG="$T/fprintd.log"
: > "$FPRINTD_LOG"
start_background "$HOWDY_PYTHON" -I "$TEST_DIR/fake_fprintd.py"
FPRINTD_PID=$BACKGROUND_PID
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
expect "a missing reader may come back: a failed attempt" "$(result_of)" 7
expect "nothing to use: nothing is said to the user" "$(grep -c '^MSG' <<< "$OUT" || true)" 0

scenario "no fingerprints, no face model" no-prints no
expect "nothing to use with enrolled fingerprints missing: nothing is said" "$(grep -c '^MSG' <<< "$OUT" || true)" 0
expect "and it does not take long" "$([ "$SECONDS_TAKEN" -lt 3 ] && echo yes || echo no)" yes
expect "nothing that can be used: not available" "$(result_of)" 9

# The face can not be used (no model), its result does not hide the one of the finger that does not match
scenario "wrong finger" nomatch-slow-stop no
expect "no face model, a wrong finger: the result of the fingerprint" "$(result_of)" 11

# The face has an error (its daemon never comes, 20 seconds), the finger does not match: the failed attempt
# is the result, not the error of the face
stop_daemon
scenario "wrong finger, no daemon" nomatch-slow-stop yes
expect "no daemon, a wrong finger: the failed attempt, not the error of the face" "$(result_of)" 11
start_background "$T/daemon"
DAEMON_PID=$BACKGROUND_PID
wait_for_name org.boltgolt.howdy.authentication

# The global timeout ends the wait for the finger. The fingerprint says that it was cancelled, the module
# that the attempt has failed.
echo "auth required $PAM_DIR/pam_howdy.so max-tries=1 global-timeout=2" > "$CONFDIR/howdy-test"
scenario "global timeout" wait no
expect "the global timeout, a cancelled fingerprint: a failed attempt" "$(result_of)" 7
echo "auth required $PAM_DIR/pam_howdy.so max-tries=1 init-message" > "$CONFDIR/howdy-test"

# A reader that is stuck (it does not answer Claim) does not hold the end of the authentication: the face
# is done, the thread of the fingerprint is asked to stop and gives its call up
# The Claim that was given up is put back: fprintd may have claimed the reader
: > "$FPRINTD_LOG"
scenario "stuck reader" stuck-claim yes
expect "a stuck reader: authenticated by the face" "$(result_of)" 0
expect "a stuck reader: the authentication is not held up by the reader" "$([ "$SECONDS_TAKEN" -lt 4 ] && echo yes || echo no)" yes
expect "a stuck reader: the Claim that was given up is released" "$(grep -c '^Release$' "$FPRINTD_LOG")" 1

# fprintd is lost during the verification (the reader was reset): that is the failure of the fingerprint
# and the others go on, the face authenticates. Nothing is sent to an fprintd that is gone.
: > "$FPRINTD_LOG"
scenario "fprintd lost" lose yes
expect "fprintd lost: the face authenticates" "$(result_of)" 0
expect "fprintd lost: the face is not held up" "$([ "$SECONDS_TAKEN" -lt 4 ] && echo yes || echo no)" yes
expect "fprintd lost: the verification was started" "$(grep -c '^VerifyStart$' "$FPRINTD_LOG")" 1
kill "$FPRINTD_PID" 2> /dev/null || true
wait "$FPRINTD_PID" 2> /dev/null || true
start_background "$HOWDY_PYTHON" -I "$TEST_DIR/fake_fprintd.py"
FPRINTD_PID=$BACKGROUND_PID
wait_for_name net.reactivated.Fprint

# fprintd is replaced during the verification, by a new one that takes the name over at once (it was
# restarted faster than it is noticed that it was gone). The verification does not go on there: the
# fingerprint is over at once and not at the timeout (6 seconds), and no signal ends the program.
echo wait > "$FPRINTD_MODE_FILE"
rm -f "$MODEL"
started=$SECONDS
run_pam_in_background
sleep 1
start_background env FPRINTD_REPLACE=1 "$HOWDY_PYTHON" -I "$TEST_DIR/fake_fprintd.py"
replacing_pid=$BACKGROUND_PID
status=0
wait "$pam_pid" || status=$?
OUT=$(cat "$T/pam_background.out")
expect "fprintd replaced: the fingerprint is over before its timeout" "$([ $((SECONDS - started)) -lt 5 ] && echo yes || echo no)" yes
expect "fprintd replaced: not authenticated" "$([ "$(result_of)" != 0 ] && echo yes || echo no)" yes
expect "fprintd replaced: the program is not ended by a signal" "$status" 0
kill "$FPRINTD_PID" "$replacing_pid" 2> /dev/null || true
wait "$FPRINTD_PID" "$replacing_pid" 2> /dev/null || true
start_background "$HOWDY_PYTHON" -I "$TEST_DIR/fake_fprintd.py"
FPRINTD_PID=$BACKGROUND_PID
wait_for_name net.reactivated.Fprint

# Waits until the stand-in of fprintd has got VerifyStop (it logs it, and answers a while later)
wait_for_verify_stop() {
	local _
	for _ in $(seq 100); do
		grep -q '^VerifyStop$' "$FPRINTD_LOG" && return
		sleep 0.1
	done
	expect "fprintd got VerifyStop in time" no yes
}

# fprintd is replaced after the finger matched, while the module puts the reader back (VerifyStop takes
# a while). The match stays: it is not turned into a lost fprintd.
echo match-slow-stop > "$FPRINTD_MODE_FILE"
rm -f "$MODEL"
: > "$FPRINTD_LOG"
run_pam_in_background
wait_for_verify_stop
start_background env FPRINTD_REPLACE=1 "$HOWDY_PYTHON" -I "$TEST_DIR/fake_fprintd.py"
replacing_pid=$BACKGROUND_PID
wait "$pam_pid"
OUT=$(cat "$T/pam_background.out")
expect "fprintd replaced after the match: authenticated by the finger" "$(result_of)" 0
kill "$FPRINTD_PID" "$replacing_pid" 2> /dev/null || true
wait "$FPRINTD_PID" "$replacing_pid" 2> /dev/null || true
start_background "$HOWDY_PYTHON" -I "$TEST_DIR/fake_fprintd.py"
FPRINTD_PID=$BACKGROUND_PID
wait_for_name net.reactivated.Fprint

# fprintd is replaced after a try that did not match, while the module puts the reader back. The next try is
# not made at the new fprintd (the reader is not claimed there): the fingerprint is over, with one VerifyStart.
echo "auth required $PAM_DIR/pam_howdy.so fingerprint:max-tries=2" > "$CONFDIR/howdy-test"
echo nomatch-slow-stop > "$FPRINTD_MODE_FILE"
rm -f "$MODEL"
: > "$FPRINTD_LOG"
run_pam_in_background
wait_for_verify_stop
start_background env FPRINTD_REPLACE=1 "$HOWDY_PYTHON" -I "$TEST_DIR/fake_fprintd.py"
replacing_pid=$BACKGROUND_PID
wait "$pam_pid"
OUT=$(cat "$T/pam_background.out")
expect "fprintd replaced between two tries: not authenticated" "$([ "$(result_of)" != 0 ] && echo yes || echo no)" yes
expect "fprintd replaced between two tries: the second try is not made" "$(grep -c '^VerifyStart$' "$FPRINTD_LOG")" 1
echo "auth required $PAM_DIR/pam_howdy.so max-tries=1" > "$CONFDIR/howdy-test"
kill "$FPRINTD_PID" "$replacing_pid" 2> /dev/null || true
wait "$FPRINTD_PID" "$replacing_pid" 2> /dev/null || true
start_background "$HOWDY_PYTHON" -I "$TEST_DIR/fake_fprintd.py"
FPRINTD_PID=$BACKGROUND_PID
wait_for_name net.reactivated.Fprint

# The daemon of the face does not answer StartAuthentication (it is stopped, as slow after the computer woke
# up) and the finger matches: the module does not wait for the face, it gives the call up
echo match > "$FPRINTD_MODE_FILE"
cp "$T/genuine.dat" "$MODEL"
chmod 600 "$MODEL"
kill -STOP "$DAEMON_PID"
started=$SECONDS
run_pam
kill -CONT "$DAEMON_PID"
expect "a face daemon that does not answer: authenticated by the finger" "$(result_of)" 0
expect "a face daemon that does not answer: the module is not held up by the face" "$([ $((SECONDS - started)) -lt 6 ] && echo yes || echo no)" yes

# cancel() ends the wait for the answer of StartAuthentication (the module itself ends the thread by force
# when another method wins, so this is tested with FaceAuthenticator alone)
kill -STOP "$DAEMON_PID"
CANCEL_OUT=$("$PAM_DIR/face_cancel_test" "$USER_NAME" 1000 2>&1)
kill -CONT "$DAEMON_PID"
expect "cancel() ends the wait for a daemon that does not answer" "$(awk '{print $2}' <<< "$CANCEL_OUT")" cancelled
expect "and it does so at once" "$([ "$(awk '{print $3}' <<< "$CANCEL_OUT")" -lt 3000 ] && echo yes || echo no)" yes

# The face answers last when its daemon starts late, its notice comes after the one of the fingerprint,
# set apart from it by a blank line like in the other order
echo "auth required $PAM_DIR/pam_howdy.so max-tries=1" > "$CONFDIR/howdy-test"
echo wait > "$FPRINTD_MODE_FILE"
cp "$T/genuine.dat" "$MODEL"
chmod 600 "$MODEL"
late_daemon
expect "a late daemon and a reader: authenticated by the face" "$(result_of)" 0
expect "a late daemon and a reader: the face comes last, set apart by a blank line" "$(grep -v '^RESULT' <<< "$OUT" | sed 's/^MSG [0-9]*: //' | head -5 | tr '\n' '|')" "Fingerprint authentication...|Place your finger on the fingerprint reader||Facial authentication...|Please look directly into the camera|"

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
