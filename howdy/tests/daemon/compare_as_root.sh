#!/bin/bash
# Sourced by run_cli.sh in its second pass: the tests of what only root may do, Compare of the management
# daemon and `howdy test`. The pass runs as root in a user namespace (unshare -r), over a bus of its own
# with the polkit stand-in and the daemon, so that the daemon sees a caller with the uid 0 like the
# installed one does. The daemon is the one of the installation, nothing in it knows about the tests.
#
# The user of the models is a normal user (USER_NAME, the one that started the test): howdy does not
# work for root itself. The variables and functions of run_cli.sh are used (expect, mcall, run, ...).

echo "--- as root: the management daemon compares faces"
rm -rf "$T/models"
expect "add" "$(mcall Add ss "$USER_NAME" "$MODEL_JSON")" "b true"
expect "add the second" "$(mcall Add ss "$USER_NAME" "$SECOND_JSON")" "b true"
expect_contains "compare finds the closest model" "$(mcall Compare ssad "$USER_NAME" euclidean 3 0.1 0.2 0.5)" '"first"'
expect_contains "compare gives the distance" "$(mcall Compare ssad "$USER_NAME" euclidean 3 0.1 0.2 0.3)" "bds true 0 \"first\""
expect_contains "compare with the dot product takes the largest" "$(mcall Compare ssad "$USER_NAME" dot 3 1 1 1)" '"second"'
expect "compare skips models of another size" "$(mcall Compare ssad "$USER_NAME" euclidean 2 0.1 0.2)" 'bds false 0 ""'
expect_contains "compare refuses an unknown metric" "$(mcall Compare ssad "$USER_NAME" cosine 3 0.1 0.2 0.3)" "Unknown metric cosine"
expect_contains "compare refuses an empty face" "$(mcall Compare ssad "$USER_NAME" euclidean 0)" "Invalid face encoding"
expect "root compares for any user" "$(mcall Compare ssad nobody euclidean 3 0.1 0.2 0.3)" 'bds false 0 ""'
expect_contains "compare for an unknown user" "$(mcall Compare ssad nosuchuser_x euclidean 3 0.1 0.2 0.3)" "Unknown user"
# It is called for every frame, and polkit is not asked for it: root can read the models anyway
asked() { grep -c "org.boltgolt.howdy.Management" "$T/polkit.log" || true; }
before="$(asked)"
out="$(COMPARE_USER="$USER_NAME" "$HOWDY_PYTHON" -I "$TEST_DIR/compare_calls.py" 4 euclidean 0.1 0.2 0.3)"
expect "compare on one connection finds the model every time" "$(grep -c "first" <<< "$out")" 4
expect "polkit is not asked for compare" "$(($(asked) - before))" 0
echo deny > "$T/polkit.mode"
out="$(COMPARE_USER="$USER_NAME" "$HOWDY_PYTHON" -I "$TEST_DIR/compare_calls.py" 2 euclidean 0.1 0.2 0.3)"
expect "a refusal of polkit does not matter" "$(grep -c "first" <<< "$out")" 2
echo allow > "$T/polkit.mode"
# Many calls at once are all answered (there is no limit)
out="$(COMPARE_USER="$USER_NAME" COMPARE_CONCURRENT=1 "$HOWDY_PYTHON" -I "$TEST_DIR/compare_calls.py" 120 euclidean 0.1 0.2 0.3)"
# (not more than 120 at once: the bus lets a connection wait for 128 answers)
expect "many calls at once are all answered" "$(grep -c "first" <<< "$out")" 120

echo "--- as root: a call that is cancelled"
# howdy test gives its call up when the window is closed, while the daemon may not answer (it is busy
# or stuck). The client has to be free in a short time. The daemon is stopped for it (SIGSTOP).
link "$T/cancel_test" "${BASE_SOURCES[@]}" "$TEST_DIR/cancel_test.cc"
# (the newest: a daemon of the first pass may still be there, which this pass may not signal)
daemon_pid="$(pgrep -n -f "^$T/management")"
kill -STOP "$daemon_pid"
out="$("$T/cancel_test" "$USER_NAME" cancel)"
kill -CONT "$daemon_pid"
expect_contains "a call that is not answered is cancelled" "$out" "cancelled "
ms="${out##* }"
if [ "${ms:-99999}" -lt 1500 ]; then echo "PASS  in a short time ($ms ms)"; else echo "FAIL  it took $ms ms"; failed=1; fi
out="$("$T/cancel_test" "$USER_NAME" wait)"
expect_contains "a call that is not cancelled gets its answer" "$out" "answered first "
out="$("$T/cancel_test" "$USER_NAME" error)"
expect_contains "and an error of the daemon comes through" "$out" "error Unknown metric cosine"

if [ "$DAEMON_ONLY" != 1 ] && [ "$HAS_XVFB" = 1 ]; then
	echo "--- as root: howdy test (xvfb)"
	cp "$T/genuine.dat" "$MODEL"
	# The window never closes by itself, stop it after a few seconds
	asked_before="$(asked)"
	run xvfb-run -a timeout 12 "$HOWDY" -U "$USER_NAME" test
	expect_contains "test shows the detection" "$output" "Opening a window with a test feed"
	expect_contains "test prints the texture" "$output" "texture: "
	expect_contains "test prints the movement" "$output" "Micro-movement: "
	expect_contains "test compares the face" "$output" "similarity: "
	expect_missing "test compares through the daemon" "$output" "Could not compare"
	# Root needs no polkit for the comparisons
	expect "polkit is not asked during the test" "$(($(asked) - asked_before))" 0
	# The video is shown 1.5 times larger than the frames of the camera (360x480 here)
	if command -v xwininfo > /dev/null; then
		size="$(xvfb-run -a bash -c '"$0" -U "$1" test > /dev/null 2>&1 & pid=$!
			for _ in $(seq 40); do
				sleep 0.5
				xwininfo -name "Howdy Test" > /dev/null 2>&1 && break
			done
			# The window is made small and gets its size when the first frame is shown
			sleep 3
			xwininfo -name "Howdy Test" 2> /dev/null | grep -E "^ +(Width|Height):"
			kill $pid 2> /dev/null' "$HOWDY" "$USER_NAME" | tr -d ' \n')"
		width="${size#Width:}"
		width="${width%%Height:*}"
		height="${size##*Height:}"
		expect "the window is 1.5 times as wide as the video" "$width" 540
		# The window has a tool bar of Qt above the video
		if [ "${height:-0}" -ge 720 ] && [ "${height:-0}" -lt 840 ]; then
			echo "PASS  and as high ($height with the tool bar)"
		else
			echo "FAIL  the height of the window is $height, expected 720 and the tool bar"
			failed=1
		fi
	fi
	# A camera that is there but can not be read at first (a face authentication may still hold it) is
	# tried again, one that does not exist fails at once
	# (a file that can not be read yet: root reads every file, so it is empty at first)
	: > "$T/busy.mp4"
	write_config "$T/busy.mp4"
	( sleep 2; cp "$T/still.mp4" "$T/busy.mp4" ) &
	freeing=$!
	run xvfb-run -a timeout 12 "$HOWDY" -U "$USER_NAME" test
	expect_contains "test waits for a camera that is busy" "$output" "Waiting for the camera"
	expect_contains "and opens it when it is free" "$output" "Opening a window with a test feed"
	wait "$freeing"
	write_config /dev/does-not-exist
	started=$SECONDS
	run xvfb-run -a timeout 12 "$HOWDY" -U "$USER_NAME" test
	expect_missing "a camera that does not exist is not waited for" "$output" "Waiting for the camera"
	if [ $((SECONDS - started)) -lt 4 ]; then echo "PASS  and it fails at once ($((SECONDS - started)) s)"; else echo "FAIL  it took $((SECONDS - started)) s"; failed=1; fi
	write_config "$T/still.mp4"
fi
