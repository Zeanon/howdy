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
	<a href="https://actions-badge.atrox.dev/boltgolt/howdy/goto?ref=beta">
		<img src="https://img.shields.io/endpoint.svg?url=https%3A%2F%2Factions-badge.atrox.dev%2Fboltgolt%2Fhowdy%2Fbadge%3Fref%3Dbeta&style=flat&label=build&logo=none">
	</a>
	<a href="https://aur.archlinux.org/packages/howdy">
		<img src="https://img.shields.io/aur/votes/howdy?color=4c1&label=aur%20votes">
	</a>
</p>

Howdy provides Windows Hello™ style authentication for Linux. Use your built-in IR emitters and camera in combination with facial recognition to prove who you are.

Using the central authentication system (PAM), this works everywhere you would otherwise need your password: Login, lock screen, sudo, su, etc.

Hello all. As a recent Linux convert I was seeking out a way to use my Windows Hello for authentication because I had gotten very used to it on my laptop. I discovered this project but like many of you there were pains in getting it to properly install. There was a lot of scattered troubleshooting information out there and I found the existing documentation a bit lacking. I really want to keep this project alive and I know @boltgolt can't work on it as often as they used to so one day I sat down and wrote this very wordy but descriptive guide in an effort to consolidate a lot of these sources. This was written partially for my own documentation, but I'm also sharing it for others to use if they are also struggling. The main thing I intended to properly fix was installing dlib without breaking system packages and instead using a venv which I believe I succeeded in doing. I used it for my install on Mint 22.1 but it should work on Ubuntu/Debian just as well, lthough I have done no additional testing for either case. Keep in mind I have very, very little actual programing or Linux knowledge so a lot of this may be very janky. If there is any glaring issues with any of it please let me know. I am hoping if this is good enough it could potentially be merged into the official README but we will see I guess.

 # Installing [boltgolt/Howdy](https://github.com/boltgolt/howdy) 3.0.0 beta with Python 3.12 on Mint 22.1:

Specifically using:
Howdy Beta 3.0.0 (commit c4521c1 - Feb 2, 2025)
ZenBook UX425UG Q408UG
Kernel: 6.8.0-55-generic 

[Link to original inspiration for instructions](https://github.com/boltgolt/howdy/issues/976) (heavily modified)

### Preparation:

1. Clone the project with version 3.0.0 beta (beta branch). Clone this to an easily accessible folder somewhere where it won't be accidentally messed with. I don't think there is anything wrong with putting this somewhere in `/home/` but it caused some slight issues for me so maybe don't do that. For me, I choose to put the parent `howdy/` folder in `/opt/`. This location (`/opt/howdy/`) will  be referred to as `/path/to/project/`.

2. Edit `meson.options` in the downloaded folder and find `option('install_pam_config')` Change  `value: false` to `value: true` 
	- This will make sure the howdy Pam config is compiled properly and will be picked up by your system as an authentication method for all auth types in `/etc/pam.d/`. If you only want to use Howdy for specific auth types that can be found in the [Howdy wiki](https://github.com/boltgolt/howdy/wiki/Only-using-howdy-for-specific-authentication-types). 

3. **(Optional)** Install python-is-python3: `sudo apt install python-is-python3` Because we will be changing the path of the python binary during this build, Howdy will not require this package to function correctly. Thus, this is not essential however on distros that have separate Python and Python 3 binaries it is a nice feature to have. Very easy to uninstall if it causes problems which it shouldn't unless you also use python 2 dependent programs. 

4. dlib Installation: 
	 - Ever since Debian 12/Ubuntu 23ish (and now any distro using Python 3.12 or higher I think), the OS, by default prevents you from installing pip packages into the global environment ([PEP 668](https://peps.python.org/pep-0668/)). The proper way to go about using pip packages then is with a python virtual environment (`venv`). The build portion of this guide will go through the process of setting this up so hold off on installing dlib until then.
	- If this does not end up working later on during the Howdy Install, you can override this prevention with `pip3 install dlib --break-system-packages` but it is [not recommended](https://stackoverflow.com/questions/75608323/how-do-i-solve-error-externally-managed-environment-every-time-i-use-pip-3) as it may cause issues with essential system packages for your distro. **([don't use sudo](https://stackoverflow.com/questions/78003100/running-pip-install-in-virtual-environment-tries-to-install-packages-in-default))**

### Setup and Building:

Build and install as partially mentioned in [README](https://github.com/boltgolt/howdy/blob/beta/README.md). (modified)

> If you want to build Howdy from source, a few dependencies are required.
> 
>  Dependencies
> 
> - Python 3.6 or higher
>     - pip 
>     - setuptools
>     - wheel
>     - opencv
>     - dev
>     - venv
> - cmake
> - make
> - build-essential
> - meson version 0.64 or higher
> - ninja-build
> - INIReader (can be pulled from git automatically if not found)
> - libpam0g-dev
> - libinih-dev
> - llibevdev-dev 
> - libopencv-dev
> - libevdev
 
To install them on Debian/Ubuntu for example:

```
sudo apt-get update && sudo apt-get install -y \
python3 python3-pip python3-setuptools python3-wheel \
python3-opencv python3-dev python3-venv cmake make \
build-essential meson ninja-build libpam0g-dev \
libinih-dev libevdev-dev libopencv-dev
```

(The backslashes are only there for formatting, can be removed if they cause issues, this is all a single command)

Now we need to configure our Python virtual environment like mentioned in step 4:

```
# cd to project foler

cd /path/to/project/

# Create venv under folder named .venv/ inside project
# directory that can access system-managed python packages

python3 -m venv .venv

# Then activate it and install meson in it
source .venv/bin/activate
pip3 install meson

# Deactivate it and install system site packages
deactivate
python3 -m venv .venv/ --system-site-packages
```

 **Note:** `--system-site-packages` is used to prevent the user from having to install every minute pip dependency again in the venv although you could do that to isolate howdy's python install even more if you wanted by removing the flag and installing everything like mentioned.
 
The `.venv/` folder can be named whatever you like so long as you change your python path in the next step accordingly, `.venv/` is simply a convention.

Now go back to `meson.options` and change `'/usr/bin/python'` in `option('python_path')` to `'/path/to/project/.venv/bin/python3'`. This is a crucial step for ensuring that Howdy is pointing to the correct Python binary inside of the venv and by extension the correct pip packages.

### **Meson Setup:**

Next you need to activate the venv you just created and setup your meson build for compilation with the proper configuration.

```
source .venv/bin/activate

# (.venv) should now appear before your username in bash

meson setup build --python.install-env venv 

# If everything appears to finish without error then you are
# good to move on
```

While still in the venv, it is a good time to install the pip package dependencies (mainly dlib) as well as meson (Meson was already installed earlier but for this to work properly you should use the pip version installed separately in the venv. You may even get away with not installing it through apt although I have not tested that).
You can run pip like normal for installation **([don't use sudo](https://stackoverflow.com/questions/78003100/running-pip-install-in-virtual-environment-tries-to-install-packages-in-default))**:

```
pip3 install dlib meson elevate keyboard

# elevate and keyboard are other dependencies for howdy-gtk
# (GUI) and rubberstamps respectively, they are not needed if
# you don't plan on using either feature
```

### Build:

```
meson compile -C build
```

(All of this will create and compile your Howdy build into a folder `build` inside of `/path/to/project`)

 You can then install Howdy to your system with:
 
```
sudo meson install -C build
```

Howdy should now be installed with the main directory of note at `/usr/local/lib/x86_64-linux-gnu/howdy/` and configs at `/usr/local/etc/howdy`.

You can leave the venv by simply running:

```
deactivate
```

### Post-Install Setup:

1.  First we need to designate our IR camera: This can be done easily with `v4l-utils`. If you do not already have it it can be installed with `sudo apt install v4l-utils` and run with the command `v4l2-ctl`. You will then want to run `v4l2-ctl --list-devices`, this will list the video devices that appear in `/dev` (ex. `/dev/video0`, `/dev/video1`, etc...). In determining which video device is the correct one, `qv4l2` is another helpful tool you can use, installed it with: `sudo apt install qv4l2`, run the command `qv4l2` and a GUI should pop up showing information about the selected camera. Under `File > Open Device` you should have the option to pick from the aforementioned video devices under `/dev`, here you can select them one at a time until you find the one designated for IR. if `qv4l2` does not show you readable names for each device under "General Information", testing of each device can be done by hitting the green, "Start Capturing" button in the top left until you find the device which both captures IR and activates the IR emitter (blinking red light). If the devices captures IR but does not activate the emitter, see the note about `linux-enable-ir-emitter`
	- It is also good practice to use the device name under `/dev/v4l/by-path` instead of just `/dev` as these device IDs will never change whereas the `/dev` names (video0, video1, etc.) may occasionally change at startup. These can be looked through with `v4l2-ctl -d /dev/v4l/by-path/[fileName] -D`for each file in the directory until the correct one is found. Mine was `/dev/v4l/by-path/pci-0000:05:00.3-usb-0:3:1.2-video-index0`.

2. Now that we have found the IR camera, it's time to designate it in the Howdy config file. Typing `sudo Howdy config` should allow you to edit it but the file is also located at `/usr/local/etc/howdy/config.ini` if you wanted to edit it manually.
	- Find: `device_path = none` and replace `none` with your IR camera directory, in my case, this was `/dev/video2` 
	- You will likely also have to change `dark_threshold` to something larger, I set mine to 90 because my IR emitter isn't all that great.

3. Go into the additional folder and drop the contents of `polkit-service.override` into the polkit service by running `sudo systemctl edit polkit-agent-helper@.service` and pasting them there.

4. 
### Real Setup

After installation, Howdy needs to learn what you look like so it can recognise you later. Run `sudo howdy add` to add a face model. <br>
If you are given any errors involving missing packages, ensure dlib and the other dependencies were all installed properly and try again. It will likely ask you to run a script to download models on first attempt, this is normal procedure for dlib and it means things are working properly. Follow the rest of the instructions and let Howdy scan your face. If it says the scan was too dark, try increasing the `dark_threshold` value in the config mentioned earlier.
	 - Other troubleshooting tips can be found here [here](https://github.com/boltgolt/howdy/wiki/Common-issues#error-when-trying-to-add-a-face-model)

Now, you can add and test the face (`sudo Howdy test`). Authentication for sudo and login should work now. <br>
Check if sudo works by opening a new shell and running `sudo -i`. <br>
Please check [this wiki page](https://github.com/boltgolt/howdy/wiki/Common-issues) if you're experiencing problems or [search](https://github.com/boltgolt/howdy/issues) for similar issues.

If you're curious you can run `sudo howdy config` to open the central config file and see the options Howdy has to offer. On most systems this will open the nano editor, where you have to press `ctrl`+`x` to save your changes.

### Uninstallation

Navigate to `/path/to/project/build`, open a terminal and run:

```
sudo ninja uninstall 
```

I think this technically just runs:

```
/path/to/project/.venv/bin/meson --internal uninstall
```

due to this excerpt from `path/to/project/build/build.ninja`

```
# Suffix

build uninstall: phony meson-internal__uninstall

build meson-internal__uninstall: CUSTOM_COMMAND PHONY
 COMMAND = /opt/howdy/.venv/bin/meson --internal uninstall
 pool = console

```

but I'm not really sure.

### Notes

- If face recognition login is still having problems. You may need to manually modify `/usr/local/lib/x86_64-linux-gnu/howdy/compare.py`. Add `sys.path.append('/usr/local/lib/python3.12/dist-packages')` after the `import sys` line. You should only have to do this if you installed the pip dependencies using `pip3 install dlib --break-system-packages` instead of making a venv.

- The new version of Howdy has a GUI utility `howdy-gtk`. Python module `elevate` is needed which I talked about installing earlier. You can activate this feature with `sudo howdy-gtk`.

- [linux-enable-ir-emitter](https://github.com/EmixamPP/linux-enable-ir-emitter) provides support for infrared cameras that are not directly supported (at the very least, the kernel must recognize your infrared camera). It can almost automatically, configure any infrared camera. Use this if your IR emitter does not function when using Howdy normally.

- [Rubberstamps](https://github.com/boltgolt/howdy/wiki/Rubber-Stamp-Guide) - ([Not really ready yet](https://github.com/boltgolt/howdy/issues/657#issuecomment-1109478205)) By default Howdy will authenticate you the moment you have been successfully recognized. However, a lot of users want to have more control over the authentication flow. This is where rubber stamps can help. Rubber stamps are available in Howdy 3.0.0 and up. 
	Stamps are activated just after your face has been successfully recognized and can approve or deny the authentication. They have to be manually enabled or disabled in the config.

- There is an issue with the GNOME keyring being locked on login as mentioned in [Ubuntu 24.04 howdy 3.0.0 beta installation #976 (comment)](https://github.com/boltgolt/howdy/issues/976#issuecomment-2629274798)
	- The instructions may not work, but this [comment](https://gist.github.com/kizzard/166470fefe8fa64d2aa65e0235115318?permalink_comment_id=5306557#gistcomment-5306557) could.


#### Sources and Helpful Links:

- [GitHub - boltgolt/howdy: 🛡️ Windows Hello™ style facial authentication for Linux](https://github.com/boltgolt/howdy) 

- [GitHub - EmixamPP/linux-enable-ir-emitter: Provides support for infrared cameras that are not directly enabled out-of-the box.](https://github.com/EmixamPP/linux-enable-ir-emitter)

- ["All frames were too dark" · Issue #751 · boltgolt/howdy](https://github.com/boltgolt/howdy/issues/751) 

- [No way to get Howdy to start authentication at system start with Ubuntu 24.04 · Issue #927 · boltgolt/howdy](https://github.com/boltgolt/howdy/issues/927)

- [What is Linux PAM Module and How to configure it? - GeeksforGeeks](https://www.geeksforgeeks.org/what-is-linux-pam-module-and-how-to-configure-it/)

- [linux-enable-ir-emitter/docs/uninstallation.md at master · EmixamPP/linux-enable-ir-emitter · GitHub](https://github.com/EmixamPP/linux-enable-ir-emitter/blob/master/docs/uninstallation.md)

- [Any experience enabling Windows Hello capabilities in Linux/Debian using Howdy : r/debian](https://www.reddit.com/r/debian/comments/1597bp8/any_experience_enabling_windows_hello/)

- [IR Emitters not turning on on Lenovo S740 · Issue #269 · boltgolt/howdy · GitHub](https://github.com/boltgolt/howdy/issues/269) 

- [Uninstalling dpdk installed using ninja - Stack Overflow](https://stackoverflow.com/questions/67868677/uninstalling-dpdk-installed-using-ninja)

- [Ubuntu 24.04 howdy 3.0.0 beta installation · Issue #976 · boltgolt/howdy](https://github.com/boltgolt/howdy/issues/976#issuecomment-2686804371)

- [UnlockGnomeKeyring.md · GitHub](https://gist.github.com/kizzard/166470fefe8fa64d2aa65e0235115318)

- [pip packages install methods](https://stackoverflow.com/a/78652149)

## CLI

The installer adds a `howdy` command to manage face models for the current user. Use `howdy --help` or `man howdy` to list the available options.

Usage:
```
howdy [-U user] [-y] command [argument]
```

| Command   | Description                                   |
|-----------|-----------------------------------------------|
| `add`     | Add a new face model for a user               |
| `clear`   | Remove all face models for a user             |
| `config`  | Open the config file in your default editor   |
| `disable` | Disable or enable howdy                       |
| `list`    | List all saved face models for a user         |
| `remove`  | Remove a specific model for a user            |
| `snapshot`| Take a snapshot of your camera input          |
| `test`    | Test the camera and recognition methods       |
| `version` | Print the current version number              |

## Contributing [![](https://img.shields.io/travis/boltgolt/howdy/dev.svg?label=dev%20build)](https://github.com/boltgolt/howdy/tree/dev) [![](https://img.shields.io/github/issues-raw/boltgolt/howdy/enhancement.svg?label=feature+requests&colorB=4c1)](https://github.com/boltgolt/howdy/issues?q=is%3Aissue+is%3Aopen+label%3Aenhancement)

The easiest ways to contribute to Howdy is by starring the repository and opening GitHub issues for features you'd like to see. If you want to do more, you can also [buy me a coffee](https://www.buymeacoffee.com/boltgolt).

Code contributions are also very welcome. If you want to port Howdy to another distro, feel free to open an issue for that too.

## Troubleshooting

Any Python errors get logged directly into the console and should indicate what went wrong. If authentication still fails but no errors are printed, you could take a look at the last lines in `/var/log/auth.log` to see if anything has been reported there.

Please first check the [wiki on common issues](https://github.com/boltgolt/howdy/wiki/Common-issues) and 
if you encounter an error that hasn't been reported yet, don't be afraid to open a new issue.

## A note on security

This package is in no way as secure as a password and will never be. Although it's harder to fool than normal face recognition, a person who looks similar to you, or a well-printed photo of you could be enough to do it. Howdy is a more quick and convenient way of logging in, not a more secure one.

To minimize the chance of this program being compromised, it's recommended to leave Howdy in `/lib/security` and to keep it read-only.

DO NOT USE HOWDY AS THE SOLE AUTHENTICATION METHOD FOR YOUR SYSTEM.
