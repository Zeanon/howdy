# Howdy Face Authentication Manager

A comprehensive bash-based CLI tool for managing Howdy face authentication on Linux systems.

## Overview

This interactive bash script provides a user-friendly menu-driven interface to configure and manage an installed Howdy through a simple terminal interface. It does not install Howdy, see the README of the project for that.

## Features

### 📸 Face Model Management
- **Add** new face models with interactive camera capture
- **List** all registered face models
- **Remove** individual models by ID
- **Clear** all models at once
- Real-time camera testing

### ⚙️ Configuration Management
- **Camera device selection** with automatic device detection via `v4l2-ctl`
- **Certainty threshold** adjustment (security level)
- **Timeout** configuration
- **Enable/Disable** Howdy
- **View** current configuration
- Direct config file editing support

### 🧪 Face Recognition Testing
- Interactive face recognition testing
- Live camera feedback
- Verification of enrolled models

## Requirements

- **Operating System**: Linux (Ubuntu, Fedora, Debian, Arch, or derivatives)
- **Howdy** installed, see the README of the project
- **Privileges**: Changing the configuration needs root, start `howdy-cli` with `sudo` for these actions. Adding, listing, testing and removing face models work as a normal user, they are authorized by polkit (action `org.boltgolt.howdy.Management`)
- `v4l-utils` for the camera device selection

## Installation

### 1. Install the Command
`howdy-cli` is part of the meson build of Howdy and is installed to the `bindir` together with it (for `/usr/local` that is `/usr/local/bin/howdy-cli`). To install only the manager, set up the build and install the files with the tag `cli_manager`, nothing has to be compiled for it:
```bash
git clone <repository-url>
cd howdy
meson setup build
sudo meson install -C build --no-rebuild --tags cli_manager
```

The prefix and the config path of the build (`-Dprefix=...`, `-Dconfig_dir=...`) are written into the script, by default the config file is `<config_dir>/config.ini`. The manager is not installed by the meson build with `-Dinstall_howdy_cli=false`.

### 2. Run It
```bash
howdy-cli
```

Changing the configuration needs root, start the manager with `sudo howdy-cli` for these actions. Without root these menu entries stop with a message and return to the menu. For the face models (add, list, test, remove) polkit asks for your authorization if it is needed. If it is denied, the action ends and the menu is shown again.

## Usage

### Main Menu

When you run the script, you'll see an interactive menu:

```
======================================
     HOWDY FACE AUTH MANAGER (BASH)
======================================

A) Add face model
T) Test face recognition
L) List face models
D) Remove / clear models
E) Enable / disable Howdy
C) Configure Howdy
Q) Exit
```

Enter the letter of the entry (upper or lower case). If an action fails, the message stays on the screen and the menu is shown below it.

### First-Time Setup

1. **Configure Camera** (`C`, then `1`, needs root)
   - View all available camera devices
   - Select your IR camera (e.g., `/dev/video0`)
   - Camera path is saved to Howdy config

2. **Add Face Model** (`A`)
   - Look at the camera when prompted
   - Your face will be captured and enrolled
   - Model is saved for authentication

3. **Test Recognition** (`T`)
   - Verify that Howdy can recognize your face
   - Get immediate feedback on recognition success

### Configuration Options

The **Configure Howdy** submenu (`C`) provides:

- `1` **Set camera device** - Change the camera used for recognition
- `E` **Enable Howdy** - Activate face authentication
- `D` **Disable Howdy** - Turn off face authentication completely
- `T` **Set timeout** - Configure how long to wait for face detection
- `S` **Set certainty** - Adjust security level in percent, 0 to 100 (higher = stricter, default 75)
- `C` **Show current config** - View the entire configuration file
- `B` **Back** - Return to the main menu

All entries that change the configuration need root.

### Managing Face Models

- **List models** (`L`): View all enrolled face models
- **Remove models** (`D`): 
  - Enter a specific model ID to remove one model
  - Enter `all` to clear all models at once

### Enable/Disable Howdy

`E` provides two choices (disabling needs root):
1. **Disable completely** - Turn off Howdy for all authentication
2. **Enable Howdy** - Instructions to re-enable via config file

## Technical Details

### Script Structure

The script is organized into logical sections:

- **Helpers**: Error handling, root privilege checking, pause functionality
- **Polkit**: Authorization for the face model commands (`authorize`)
- **Menu actions**: Every action runs in its own subshell (`run_action`), a failing command ends the action, not the manager
- **Camera Configuration**: Device detection and configuration
- **Menu Actions**: Face model management, testing, configuration
- **Main Menu**: Interactive menu loop

### Configuration File

Howdy configuration is stored in the config directory that meson set when the manager was built (by default `/usr/local/etc/howdy/config.ini`). The script provides helper functions to safely modify configuration values without manual editing.

### Key Functions

- `require_root()` - Stops an action that needs root privileges
- `authorize()` - Asks polkit for the management action once
- `run_action()` - Runs a menu action and returns to the menu if it fails
- `configure_camera()` - Sets up camera device
- `add_face()` - Enrolls new face model
- `test_face()` - Tests face recognition
- `config_menu()` - Interactive configuration interface
- `set_config()` - Safely updates config file values

## Troubleshooting

### Script won't run
```bash
# Install the command again
sudo meson install -C build --no-rebuild --tags cli_manager

# Run it, with sudo for configuring
sudo howdy-cli
```

### Camera not detected
```bash
# List available cameras manually
v4l2-ctl --list-devices

# Check if your IR camera is recognized
ls -l /dev/video*
```

### Face recognition not working
- Ensure IR camera is properly connected
- Verify camera device path in config (`C` → `C`)
- Add multiple face models for different lighting conditions
- Adjust certainty threshold (higher = stricter, 0 to 100, default 75)
- Test in good lighting conditions

### Permission errors
- Changing the configuration needs root: start the manager with `sudo howdy-cli`
- Adding, listing and removing face models ask polkit for an authorization. If you deny it, the action ends and the menu is shown again
- Check that the config file (by default `/usr/local/etc/howdy/config.ini`) is writable for root

### Configuration not saving
```bash
# Verify config file exists
ls -l /usr/local/etc/howdy/config.ini

# Check file permissions
sudo chmod 644 /usr/local/etc/howdy/config.ini
```

## Known Limitations

- Changing the configuration needs root
- IR camera required for face authentication
- Configuration changes require script restart to take effect

## Security Considerations

- **Certainty threshold**: Higher values (0 to 100, default 75) provide stricter authentication, but may reject you in poor light
- **Multiple models**: Register your face in different conditions (glasses, no glasses, different lighting)
- **Timeout**: Shorter timeouts reduce the window for unauthorized access attempts

## Advanced Usage

### Direct Config Editing
```bash
# Edit config file directly
sudo nano /usr/local/etc/howdy/config.ini

# Or use Howdy's built-in editor
sudo howdy config
```

### Manual Howdy Commands
```bash
# Add face model
sudo howdy add

# List models
sudo howdy list

# Remove specific model
sudo howdy remove <id>

# Clear all models
sudo howdy clear

# Test recognition
sudo howdy test

# Disable Howdy
sudo howdy disable
```

## Project Structure

```
howdy-cli/
├── bin/howdy-manager.sh.in # Main bash script (template, meson sets the config path)
├── meson.build             # Installs the script as <bindir>/howdy-cli
└── README.md               # This file
```

## Contributing

Contributions are welcome! Please ensure:
- Code follows bash best practices
- Error handling is comprehensive

## License

This project is provided as-is for managing Howdy face authentication.

## Credits

- **Howdy**: [boltgolt/howdy](https://github.com/boltgolt/howdy) - The underlying face authentication system
- **v4l-utils**: Video4Linux utilities for camera management

## Resources

- [Howdy GitHub Repository](https://github.com/boltgolt/howdy)
- [Howdy Documentation](https://github.com/boltgolt/howdy/wiki)

## Changelog

### Current Version
- Interactive bash-based CLI tool
- Comprehensive configuration management
- Face model management (add, list, remove, clear)
- Camera device configuration with auto-detection
- Enable/disable functionality
- Interactive testing interface
