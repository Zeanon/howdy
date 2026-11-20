# Tests for the C++ components

Four test suites check the C++ authentication daemon (`howdy/src/cpp/daemon`, built on `howdy/src/cpp/core` and `howdy/src/cpp/common`), the
C++ management daemon (`howdy/src/cpp/management`), the C++ command line interface
(`howdy/src/cpp/cli`) and the graphical interface (`howdy/src/cpp/gui`), which is
part of the same program (`howdy gui`, also started as `howdy-gtk`). None of
them needs root, a camera or the system bus.

| Script | What it checks |
|---|---|
| `run_parity.sh` | Detections, landmarks, encodings and liveness/spoof scores of the C++ code compared with the Python code on the dlib example images (dlib HOG, dlib CNN and, if available, OpenVINO). dlib has to be bit identical, OpenVINO within float rounding. |
| `run_e2e.sh` | The authentication daemon over a private D-Bus: success, foreign face, missing model/camera, invalid arguments, unknown user, busy, cancel, clear, keepalive timeout, popup signals, liveness rejecting a photo and a clean shutdown. A video made from a still image is used as camera through the ffmpeg recorder. |
| `run_cli.sh` | The management daemon and the `howdy` command over a private D-Bus with a stand-in for polkit: the D-Bus API (add, list, remove, clear, invalid and oversized models, other users, polkit denying), D-Bus activation, every command of the cli (`list`, `remove`, `clear`, `disable`, `set`, `config`, `snapshot`, `test`) and the guided capture of `howdy add` in a virtual display (xvfb). `disable`, `set` and `config` run in a user namespace, which is root for them without being root. |
| `run_snapshot.sh` | The retention of the snapshots (`retention`, `max_snapshots` in the config): `prune_snapshots()` deletes by age and by number, keeps the newest, leaves other files, links and a linked directory alone. `run_e2e.sh` additionally checks that the daemon deletes old snapshots when it starts and keeps only the newest ones of failed attempts. |
| `run_trusted_file.sh` | `read_trusted_file()`, which decides if a model file is used: a file of root or of the user of the daemon is, one that the group or others can write, a link, a directory or a file in a directory that others can write is not. `run_e2e.sh` and `run_cli.sh` check that the daemons refuse such a file. |
| `run_pam.sh` | `pam_howdy.so` in a real PAM stack (`pam_start_confdir`) against the authentication daemon on a private D-Bus, in a user namespace because the module only reads a configuration of root: a user without a face model is not asked to look into the camera and is not made to wait, the genuine face authenticates and is told so, a foreign face does not. With a stand-in for fprintd the module only announces what can be used for the user (face model, enrolled fingerprints, reader) in the messages and the `init-message`, and says nothing if neither can be used. |
| `run_python.sh` | The checks who calls the Python daemons, without a bus or polkit (a stand-in answers which user a bus name belongs to): the common caller checks, who may see, cancel and clear a request of the authentication daemon and who may start one, the caller of the management daemon and the polkit check of the cli package, the retention of the snapshots and `read_trusted_file()` of the Python daemons. Needs only the reference Python with `dbus-next`, opencv and numpy. |
| `run_gui.sh` | The graphical interface in a virtual display: `gui_test.cc` clicks the buttons of the window and the setup wizard. The management daemon is a stand-in on a private D-Bus that logs which connection every call came from, to show that all calls come from one process (polkit then asks for the password once). Covered are adding a model (loading the models, the dialog, saving it), deleting, every failure, the refusal by polkit, closing the dialog (clicked from outside with `xdotool`), the whole wizard with infrared and other cameras, the downloads and the certainty policies, and the program itself started as `howdy gui` and as `howdy-gtk`. A program called `howdy` that logs its calls shows that the window starts no subprocess. `CAMERA=/dev/video0` additionally tests the video tab with a real camera (nothing is saved). |

The CI runs `run_trusted_file.sh`, `run_snapshot.sh` and `run_python.sh` (with `SKIP_IMAGES=1` and the Python of the system), the others need
models, a camera stand-in, a bus or a display.

`SLOW_TESTS=1 howdy/tests/daemon/run_cli.sh` additionally runs `howdy add` with
the real timings (about a minute) and waits for the idle timeout of the
management daemon.

## Requirements

- The build dependencies of the daemon (`libdlib-dev`, `nlohmann-json3-dev`,
  `libopencv-dev`, `libinih-dev`, `libsystemd-dev`) and optionally OpenVINO
- A Python with `dlib`, `opencv` and (for the OpenVINO tests) `openvino` as
  reference, by default the Howdy venv
- The dlib and OpenVINO model files
- `curl` (the example images are downloaded from the dlib repository)
- For `run_e2e.sh` additionally `dbus-daemon`, `dbus-monitor`, `busctl` and `ffmpeg`
- For `run_gui.sh` additionally `dbus-daemon`, `busctl`, `ffmpeg`, `xvfb-run`, `xdotool`,
  `xprop` and `dbus-next` in the reference Python (stand-in for the daemon)
- For `run_cli.sh` additionally `dbus-daemon`, `busctl`, `ffmpeg`, `unshare`,
  `dbus-next` in the reference Python (stand-in for polkit) and, for the
  tests of the dialog and the test window, `xvfb-run`; without it these are
  skipped. GTK 3 and the OpenCV `highgui` module are needed to build the cli.

## Usage

```sh
howdy/tests/daemon/run_parity.sh
howdy/tests/daemon/run_e2e.sh
howdy/tests/daemon/run_cli.sh
howdy/tests/daemon/run_gui.sh
```

Both exit with a non-zero status if a check fails.

## Environment

| Variable | Default |
|---|---|
| `WORK_DIR` | new temporary directory |
| `HOWDY_PYTHON` | `/usr/local/lib/x86_64-linux-gnu/Howdy/.venv/bin/python3` |
| `DLIB_DATA_DIR` | `/usr/local/share/howdy/dlib-models` |
| `OPENVINO_DATA_DIR` | `/usr/local/share/howdy/openvino-models` |
| `OPENVINO_DIR` | unset, then `pkg-config openvino` is tried. Set it to an OpenVINO runtime with `include/` and `libs/`, e.g. the `openvino` directory of the pip wheel. |
| `CXX` | `g++` |
| `SANITIZE` | unset, `1` builds with AddressSanitizer and UBSan |

Example with OpenVINO from the Howdy venv:

```sh
OPENVINO_DIR=/usr/local/lib/x86_64-linux-gnu/Howdy/.venv/lib/python3.13/site-packages/openvino \
	howdy/tests/daemon/run_parity.sh
```

## Files

- `common.sh`: shared setup, configures `paths.hh` and the Python package with test paths and compiles (the objects are cached in the work directory and built in parallel)
- `bus.sh`: private D-Bus with activation of services, and the `expect` helpers
- `parity.cc`, `parity.py`, `compare.py`: parity harness for C++ and Python and the comparison
- `e2e.sh`: runs a single authentication through D-Bus and checks the status
- `fake_polkit.py`: stand-in for polkit, allows, denies or asks for authentication
- `enroll_test.cc`: runs the guided capture of `howdy add` with short timings
- `gui_test.cc`, `fake_management.py`: the driver of `run_gui.sh` and the stand-in for the management daemon
- `dialog_check.sh`: looks at the enrollment dialog from outside (its parent window, clicking "Cancel")
- `crop_face.py`: cuts one face out of an example image for the enrollment tests
- `mkconf.py`, `mkmodel.py`: write a test config and a face model
- `fetch_images.sh`: downloads the dlib example images
