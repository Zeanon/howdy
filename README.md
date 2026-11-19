![](https://boltgolt.nl/howdy/banner.png)

<p align="center">
	<a href="https://github.com/boltgolt/howdy/releases">
		<img src="https://img.shields.io/github/release/boltgolt/howdy.svg?colorB=4c1">
	</a>
	<a href="https://github.com/boltgolt/howdy/graphs/contributors">
		<img src="https://img.shields.io/github/contributors/boltgolt/howdy.svg?style=flat">
	</a>
	<a href="https://www.buymeacoffee.com/boltgolt">
		<img src="https://img.shields.io/badge/endpoint.svg?url=https://boltgolt.nl/howdy/shield.json">
	</a>
	<a href="https://aur.archlinux.org/packages/howdy">
		<img src="https://img.shields.io/aur/votes/howdy?color=4c1&label=aur%20votes">
	</a>
</p>

Howdy provides Windows Hello™ style authentication for Linux. Use your built-in IR emitters and camera in combination with facial recognition to prove who you are.

Using the central authentication system (PAM), this works everywhere you would otherwise need your password: login, lock screen, sudo, su, polkit, etc.

## Contents

- [How it works](#how-it-works)
- [Installation](#installation)
- [Setup](#setup)
- [Configuration](#configuration)
- [Command line](#command-line)
- [Troubleshooting](#troubleshooting)
- [Uninstallation](#uninstallation)
- [Contributing](#contributing)
- [A note on security](#a-note-on-security)

## How it works

Howdy consists of a few cooperating components:

| Component | Description |
|-----------|-------------|
| `pam_howdy.so` | The PAM module. It asks the authentication daemon to verify the user and can also run a fingerprint or password prompt in parallel. |
| `howdy-authentication-daemon` | Runs as a systemd service, keeps the models loaded and does the camera capture, face detection, recognition and the anti-spoofing checks. |
| `howdy-management-daemon` | Stores and manages the face models of the users. Accessed through D-Bus and polkit. |
| `howdy` | The command line tool. It also contains the configuration window (`howdy gui`, also available as `howdy-gtk`). |
| `howdy-auth-popup.service` | Optional user service that shows a popup while a scan is running. |

Everything is written in C++. The authentication daemon, the management daemon, the command line tool and the configuration window each also exist as a Python implementation, which can be chosen at build time (see [Build options](#build-options)).

Two recognition backends are available:

- **dlib** (default): works everywhere. It has two face detectors, selected with `use_cnn` in `[face_authentication]`:
  - **HOG** (`use_cnn = false`, default): fast on the CPU, less accurate.
  - **CNN** (`use_cnn = true`): much more accurate, but needs a lot more power and is meant to run on a GPU to be fast enough. It uses the `mmod_human_face_detector` model.
- **OpenVINO**: runs on Intel iGPUs / CPUs and is a lot faster. Needs additional models and the OpenVINO runtime at build time.

## Installation

Howdy is packaged for Debian/Ubuntu, Arch Linux, Fedora and openSUSE. If you are interested in packaging Howdy for your distro, don't hesitate to open an issue. The packages are maintained separately, so they may not offer every build option described in this document.

**Note:** The build of dlib can hang on 100% for over a minute, give it time.

### Packages

#### Ubuntu or Linux Mint

Run the installer by pasting (`ctrl+shift+V`) the following commands into the terminal one at a time:

```
sudo add-apt-repository ppa:boltgolt/howdy
sudo apt update
sudo apt install howdy-gtk
```

This  installs Howdy together with its graphical interface. Once installed, run this command to use the installation wizard:

```
howdy-gtk
```

#### Debian

Download the .deb file from the [Releases page](https://github.com/boltgolt/howdy/releases) and install it with apt from the folder you downloaded it to:

```
sudo apt install ./howdy_*.deb
```

#### Arch Linux

Install the `howdy` package from the AUR. For AUR installation instructions, take a look at this [wiki page](https://wiki.archlinux.org/index.php/Arch_User_Repository#Installing_packages).

You will need to do some additional configuration steps. Please read the [ArchWiki entry](https://wiki.archlinux.org/index.php/Howdy) for more information.

#### Fedora

The `howdy` package is available as a [Fedora COPR repository](https://copr.fedorainfracloud.org/coprs/alvarogarinf/howdy/), install it by simply executing the following commands in a terminal:

```
sudo dnf copr enable alvarogarinf/howdy
sudo dnf --refresh install howdy
```

See the link to the COPR repository for detailed configuration steps.

#### openSUSE

Go to the [openSUSE wiki page](https://en.opensuse.org/SDB:Facial_authentication) for detailed installation instructions.

### Building from source

If there is no package for your distribution, or you want to change the build options, build Howdy from source as described in the following sections.

### Dependencies

Building Howdy needs a C++20 compiler, the following build tools and development packages:

- meson (version 1.1.0 or higher), ninja, pkg-config
- Python 3 (used by meson, and by the optional Python components)
- PAM, libevdev, libinih (INIReader), libsystemd
- GTK 3 (version 3.24 or higher), cairo and GIO
- OpenCV 4
- dlib, BLAS and LAPACK
- nlohmann-json
- `wget` or `curl` and `bzip2` to download the models
- `v4l-utils` to find your IR camera (recommended)

On Debian/Ubuntu:

```
sudo apt-get update && sudo apt-get install -y \
	git build-essential meson ninja-build pkg-config python3 python3-venv \
	libpam0g-dev libevdev-dev libinih-dev libsystemd-dev libgtk-3-dev \
	libopencv-dev libdlib-dev libblas-dev liblapack-dev nlohmann-json3-dev \
	wget bzip2 v4l-utils
```

Package names differ on other distributions. If `INIReader` is not found, meson falls back to fetching the `inih` subproject.

#### OpenVINO (optional)

OpenVINO is picked up automatically (`-Dopenvino=auto`), in this order:

1. A system wide installation (CMake config or pkg-config)
2. The runtime of the `openvino` pip wheel in the Python environment configured with `python_path`

To use the pip wheel, install it into the Howdy virtual environment, see [Python environment](#python-environment).

Use `-Dopenvino=disabled` to build without it, or `-Dopenvino=enabled` to fail if it can't be found.

### Python environment

Meson needs a Python interpreter, and Howdy installs its Python code and the Python components into the virtual environment (`venv`) behind `python_path`. The default is `<install_dir>/.venv/bin/python3`, e.g. `/usr/local/lib/x86_64-linux-gnu/Howdy/.venv/bin/python3`. You need it if you

- use any Python implementation (`-Dauth_daemon=python`, `-Dmanagement_daemon=python`, `-Dcli=python` or `-Dgtk_app=python`),
- want OpenVINO from the `openvino` pip wheel, or
- use the Python implementation of the authentication daemon together with the `hotkey` rubber stamp (the C++ daemon has its own implementation).

The environment has to exist **before** `meson setup`, because the interpreter is looked up then. Modern distributions refuse to install pip packages into the system Python ([PEP 668](https://peps.python.org/pep-0668/)), so a venv is the proper way. Don't use `--break-system-packages`, and always call the pip of the venv (not the system `pip`), also when using `sudo`.

1. Create an isolated environment where `python_path` points to. It deliberately does **not** use `--system-site-packages`: Howdy runs as root, and a venv that is cut off from the system packages can't be influenced by anything installed globally or in a user directory (no dependency injection). Everything the Python components import comes from this one directory, which only root can write to.

   ```
   sudo apt-get install -y python3-venv python3-dev
   sudo python3 -m venv /usr/local/lib/x86_64-linux-gnu/Howdy/.venv
   ```

   To keep it elsewhere, create it there and pass it to meson with `-Dpython_path=/path/to/.venv/bin/python3`. Keep that directory owned by root and not writable by other users.

2. Install **all** Python dependencies into it with the pip of the venv. Nothing is taken from the system, so numpy, OpenCV and PyGObject need to be installed too:

   ```
   VENV_PIP=/usr/local/lib/x86_64-linux-gnu/Howdy/.venv/bin/pip

   # Needed to compile PyGObject (GUI, popup, enrollment dialog) and dlib
   sudo apt-get install -y build-essential cmake pkg-config \
   	libgirepository1.0-dev libcairo2-dev gir1.2-gtk-3.0 \
   	libblas-dev liblapack-dev

   sudo $VENV_PIP install numpy opencv-python dbus-next PyGObject pycairo

   # Python implementation of the dlib backend (compiles dlib, takes a while)
   sudo $VENV_PIP install dlib

   # OpenVINO backend, either as the C++ runtime or for the Python daemon
   sudo $VENV_PIP install openvino

   # Hotkey rubber stamp of the Python authentication daemon
   sudo $VENV_PIP install keyboard
   ```

   Install only what you use. If you only need the OpenVINO runtime for the C++ daemon, `openvino` is enough. PyGObject loads the GTK typelibs (`gir1.2-gtk-3.0`) from the system, those are data files, not Python code. `opencv-python` ships its own OpenCV, if you only need the daemon without `howdy test` windows, `opencv-python-headless` works as well.

   Pin the versions (`pip install numpy==X.Y.Z ...`, or a `--require-hashes` requirements file) if you want reproducible installs and protection against a tampered package update.

3. Configure meson so it installs into that environment, then build and install as described above:

   ```
   meson setup build --python.install-env venv
   ```

   With a venv at the default location this is already what `./install.sh --reconfigure` does.

Reinstalling Howdy replaces the `howdy` package in the venv. `install.sh` removes the old copies first, so no stale files stay behind. If your Python version changes (e.g. after a distribution upgrade), recreate the venv and reinstall.

### Build

Clone the repository to somewhere it won't be accidentally modified, then configure and compile:

```
git clone https://github.com/boltgolt/howdy.git
cd howdy

meson setup build --python.install-env venv
meson compile -C build
```

### Install

The repository contains an `install.sh` script that compiles, installs and (re)starts everything:

```
./install.sh
```

It calls `sudo` where needed. In detail it:

- rebuilds the project and the additional files
- creates the user models directory with the right permissions
- runs `meson install`
- enables and (re)starts `howdy-authentication.service`
- enables the `howdy-auth-popup.service` user service

Add `--reconfigure` to remove and recreate the build directory first.

At the end the script tells you to run the model installers if the face models are not installed yet, see below. Without the installer script you can also install manually with `sudo meson install -C build`.

Howdy is installed to `/usr/local/lib/<multiarch>/Howdy`, the configuration to `/usr/local/etc/howdy`, the PAM module to `/usr/lib/security` and the models to `/usr/local/share/howdy`. All of these locations can be changed, see [Build options](#build-options).

### Models

The models are not part of the repository and have to be downloaded once. The installer scripts are placed next to the models directories during installation.

**dlib** (required for the default backend):

```
sudo /usr/local/share/howdy/dlib-models/install.sh
```

This downloads `dlib_face_recognition_resnet_model_v1`, `mmod_human_face_detector` and `shape_predictor_5_face_landmarks`. The `mmod_human_face_detector` is only used with `use_cnn = true`, but the installer always downloads all three.

**OpenVINO** (only if you want to use it):

```
sudo /usr/local/share/howdy/openvino-models/install.sh
```

This downloads the face detection, re-identification, landmark and anti-spoofing models.

`install.sh` prints a reminder after an installation, but only if one or more of these models are missing.

### Build options

Options are passed to `meson setup` (or `meson configure build`), for example `meson setup build -Dopenvino=disabled`.

| Option | Default | Description |
|--------|---------|-------------|
| `install_dir` | `<prefix>/lib/<multiarch>/Howdy` | Where the daemons and the Python sources are installed |
| `pam_dir` | `/usr/lib/security` | Where `pam_howdy.so` is installed |
| `config_dir` | `<prefix>/etc/howdy` | Configuration directory |
| `user_models_dir` | `<config_dir>/models` | Where the face models are stored |
| `dlib_data_dir` | `<prefix>/share/howdy/dlib-models` | dlib model directory |
| `openvino_data_dir` | `<prefix>/share/howdy/openvino-models` | OpenVINO model directory |
| `openvino_cache_dir` | `/var/cache/howdy/openvino` | Cache for the compiled OpenVINO model blobs |
| `log_path` | `/var/log/howdy` | Log and snapshot directory |
| `python_path` | `<install_dir>/.venv/bin/python3` | Python used by meson and the Python components |
| `install_config` | `true` | Install the default `config.ini` (an existing one is never overwritten) |
| `install_pam_config` | `true` | Install the PAM config for `pam-auth-update` (Debian/Ubuntu) |
| `with_wakeup_hook` | `true` | Restart Howdy automatically after the system woke up |
| `openvino` | `auto` | OpenVINO support in the authentication daemon |
| `auth_daemon` | `cpp` | `cpp` or `python` implementation of the authentication daemon |
| `management_daemon` | `cpp` | `cpp` or `python` implementation of the management daemon |
| `cli` | `cpp` | `cpp` or `python` implementation of the command line tool |
| `gtk_app` | `cpp` | `cpp` builds the configuration window into `howdy` (needs `-Dcli=cpp`), `python` installs the Python app |

## Setup

### 1. Find your IR camera

List the video devices with:

```
v4l2-ctl --list-devices
```

Usually one of them is the IR camera. `qv4l2` helps to identify it: open each device and press "Start Capturing" until you find the one that shows a grayscale image and makes the IR emitter blink (a red light).

Prefer the stable name in `/dev/v4l/by-path/` over `/dev/videoN`, since the latter can change between boots. Inspect one with `v4l2-ctl -d /dev/v4l/by-path/<name> -D`.

If the camera shows an image but the emitter doesn't light up, take a look at [linux-enable-ir-emitter](https://github.com/EmixamPP/linux-enable-ir-emitter).

### 2. Configure Howdy

Open the configuration with `sudo howdy config` (or edit `/usr/local/etc/howdy/config.ini`) and set at least:

- `device_path` in `[video]` to the camera from step 1
- `dark_threshold` in `[video]` if scans are rejected as too dark. A larger value accepts darker images.

You can also use the setup wizard of the configuration window: `howdy gui --force-onboarding`.

### 3. Give the daemons camera access

The authentication daemon runs sandboxed. Copy the contents of `additional/howdy-authentication-service.override` to the service and adjust the `DeviceAllow` lines for your camera and GPU:

```
sudo systemctl edit howdy-authentication.service
```

If you want to use polkit prompts (e.g. for the lock screen on some desktops), also add `additional/polkit-service.override` to the polkit agent helper:

```
sudo systemctl edit polkit-agent-helper@.service
```

`additional/install-additional.sh` installs optional PAM files for KDE's fingerprint prompt and polkit. It asks before changing anything and offers to back up existing files.

### 4. Add your face

```
sudo howdy add
```

This opens a dialog that guides you through small head movements. Several descriptors are stored per model, which makes recognition more reliable than a single snapshot. If a scan is too dark, raise `dark_threshold` as described above.

You can add multiple models, for example with and without glasses. Give them a label when asked.

### 5. Test it

```
sudo howdy test
```

shows the camera feed with detected faces, the recognition values and the state of the anti-spoofing checks. Then open a new terminal and run `sudo -i` to try it for real. Keep your original terminal open until you are sure everything works.

### Enabling OpenVINO

1. Build with OpenVINO support (see [OpenVINO](#openvino-optional)) and install the [OpenVINO models](#models)
2. Set `use_openvino = true` in `[face_authentication]`
3. Select the device with `openvino_device` in `[video]` (`GPU`, `CPU`, ...)
4. Run `sudo howdy add` again. Models are created by one backend and can't be used by the other.

The first start compiles the models for your device. The result is cached in `openvino_cache_dir`.

## Configuration

All options are documented in the comments of `config.ini`. Some highlights:

| Section | Option | Description |
|---------|--------|-------------|
| `[core]` | `disabled` | Turn Howdy off (also available as `howdy disable 1`) |
| `[core]` | `abort_if_ssh`, `abort_if_lid_closed` | Skip face recognition in these situations |
| `[face_authentication]` | `certainty` | How strict the match is. Lower is stricter. |
| `[face_authentication]` | `use_cnn` | Use the more accurate dlib CNN detector instead of HOG. Needs a GPU to be fast. Ignored with OpenVINO. |
| `[face_authentication]` | `use_openvino` | Use the OpenVINO backend |
| `[face_authentication]` | `use_anti_spoof`, `use_damo_flir`, `micromotion_detection`, `texture_detection`, `similarity_detection` | Liveness checks against photos and screens |
| `[fingerprint]`, `[password]` | `enable` | Run a fingerprint or password prompt in parallel with the face scan |
| `[video]` | `device_path` | The IR camera |
| `[video]` | `dark_threshold` | Frames darker than this are skipped |
| `[video]` | `rotate` | Rotation of the captured frames, see below |
| `[snapshots]` | `save_failed`, `save_successful` | Save snapshots of login attempts to `log_path/snapshots` |
| `[rubberstamps]` | `enabled`, `stamp_rules` | Extra checks after a successful recognition, see the [rubber stamp guide](https://github.com/boltgolt/howdy/wiki/Rubber-Stamp-Guide) |

### Rubber stamps

Rubber stamps are extra checks that run after a successful face recognition and can still deny the authentication. Enable them with `enabled = true` in `[rubberstamps]` and list them in `stamp_rules`, one per line:

```
stamp_rules =
	nod 5s failsafe
	hotkey 10 faildeadly abort_key=esc confirm_key=ctrl+y
```

The format is `type timeout (failsafe | faildeadly) [option=value ...]`. With `failsafe` a stamp that runs into its timeout denies the authentication, with `faildeadly` it lets it pass.

| Stamp | Description | Options |
|-------|-------------|---------|
| `nod` | Nod to confirm, shake your head to abort | `min_distance` (6), `min_directions` (2) |
| `hotkey` | Press a key to confirm or abort | `abort_key` (`esc`), `confirm_key` (`enter`) |

Both daemons implement the stamps. A stamp that can't run (no camera, no keyboard) denies the authentication, a cancelled authentication stops the running stamp right away. The texts are shown in the authentication popup, so enable `show_auth_popup` to see them.

The C++ `hotkey` stamp reads the keyboards in `/dev/input` through evdev, the daemon needs access to them. Uncomment the `SupplementaryGroups` and `DeviceAllow` lines for input devices in the service override from the setup step. Keys are named like the Linux key codes (`a`, `f5`, `space`, `esc`, ...), with `ctrl`, `shift`, `alt` and `windows` as modifiers, e.g. `ctrl+alt+x`.

### Camera rotation

Some cameras deliver an image that is rotated, for example the IR camera of the Surface Pro 7+. Use `rotate` in `[video]`:

| Value | Behaviour |
|-------|-----------|
| `0` | Landscape orientation only |
| `1` | Alternates between landscape and both portrait orientations |
| `2` | Portrait orientations only, alternating the direction every frame |
| `3` | Always rotate 90° counter-clockwise |
| `4` | Always rotate 90° clockwise |

Modes 1 and 2 alternate the direction every frame, which lines up badly with IR emitters that light up every second frame. For a sideways mounted camera use `3` or `4`.

`howdy add` and `howdy test` only apply the fixed modes 3 and 4. They would flicker (test) or store models of mixed orientations (add) otherwise. A model is therefore always created from the same upright image that is used for authentication.

## Command line

```
howdy [-U user] [-y] command [argument]
```

Use `howdy --help` or `man howdy` for the full list.

| Command | Description |
|---------|-------------|
| `add` | Add a new face model for a user |
| `clear` | Remove all face models for a user |
| `config` | Open the config file in your default editor |
| `disable` | Disable or enable Howdy |
| `gui` | Open the configuration window (also started as `howdy-gtk`). `--force-onboarding` starts the setup wizard. |
| `list` | List all saved face models for a user |
| `remove` | Remove a specific model for a user |
| `set` | Change a config option |
| `snapshot` | Take a snapshot of your camera input |
| `test` | Test the camera and recognition methods |
| `version` | Print the current version number |

## Troubleshooting

- Errors of the commands are printed to the terminal. The authentication daemon logs to the journal: `journalctl -u howdy-authentication.service`. PAM related messages end up in `/var/log/auth.log` or the journal.
- **"Face models are not installed" / missing model errors:** run the model installers from the [Models](#models) section.
- **All frames are too dark:** increase `dark_threshold`, and check that the IR emitter actually lights up.
- **No camera / permission denied:** check `device_path` and that the camera is allowed in the service override (step 3).
- **The scan works on the command line but not at the login screen:** the daemon needs to be running (`systemctl status howdy-authentication.service`), and `ProtectHome` / `DeviceAllow` settings of your display manager's PAM service can matter.
- **The GNOME keyring stays locked after a face login.** Howdy can't provide the password that unlocks it. See this [workaround](https://gist.github.com/kizzard/166470fefe8fa64d2aa65e0235115318) and [issue #976](https://github.com/boltgolt/howdy/issues/976#issuecomment-2629274798), which may or may not work for you.
- More tips in the [wiki on common issues](https://github.com/boltgolt/howdy/wiki/Common-issues). If you encounter an error that hasn't been reported yet, don't be afraid to [open an issue](https://github.com/boltgolt/howdy/issues).

## Uninstallation

Run the following in the build directory:

```
sudo ninja -C build uninstall
```

Stop and disable the services before, and remove the files that meson does not track:

```
sudo systemctl disable --now howdy-authentication.service
sudo systemctl --global disable howdy-auth-popup.service
```

The face models in `/usr/local/etc/howdy/models`, the downloaded models in `/usr/local/share/howdy`, the cache in `/var/cache/howdy` and the logs in `/var/log/howdy` can be deleted manually. If you installed polkit or KDE PAM files with `install-additional.sh`, restore your backups from `/etc/pam.d/*.bak`.

## Contributing

The easiest ways to contribute to Howdy is by starring the repository and opening GitHub issues for features you'd like to see. If you want to do more, you can also [buy me a coffee](https://www.buymeacoffee.com/boltgolt).

Code contributions are also very welcome. If you want to port Howdy to another distro, feel free to open an issue for that too.

## A note on security

This package is in no way as secure as a password and will never be. Although it's harder to fool than normal face recognition, a person who looks similar to you, or a well-printed photo of you could be enough to do it. Howdy is a more quick and convenient way of logging in, not a more secure one.

To minimize the chance of this program being compromised, it's recommended to leave Howdy in `/lib/security` and to keep it read-only.

DO NOT USE HOWDY AS THE SOLE AUTHENTICATION METHOD FOR YOUR SYSTEM.
