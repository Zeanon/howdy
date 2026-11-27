#!/bin/bash
set -e

# ruff format --config "format.indent-style='tab'"

BUILD_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/build"

# A typo must not be passed over: the installation would run without what was asked for
for argument in "$@"; do
    case "$argument" in
        --reconfigure | --encrypt-models | --restore-models-key) ;;
        *)
            echo "Unknown argument: $argument" >&2
            echo "Usage: $0 [--reconfigure] [--encrypt-models] [--restore-models-key]" >&2
            echo "Build options are set with: meson configure build -Dname=value" >&2
            exit 1
            ;;
    esac
done

# Writes the options that were chosen for a build directory (meson records them in the
# file) as -D arguments, each ended by a NUL
saved_options() {
    python3 -I -c '
import configparser, sys
parser = configparser.ConfigParser(delimiters=("=",), interpolation=None)
parser.optionxform = str
parser.read(sys.argv[1])
for key, value in parser.items("options") if parser.has_section("options") else []:
    sys.stdout.write(f"-D{key}={value}\0")
' "$1"
}

# The build directory is set up when there is none yet, and anew with --reconfigure
if [[ " $* " == *" --reconfigure "* ]] || [ ! -f "$BUILD_DIR/meson-private/coredata.dat" ]; then
    echo "################################"
    echo "# Setting up the build dir     #"
    echo "################################"

    # The options of the old one are kept, without models_key_file for example the
    # services could not read the encrypted models any more
    CMD_LINE="$BUILD_DIR/meson-private/cmd_line.txt"
    WORK="$(mktemp -d)"
    trap 'rm -rf -- "$WORK"' EXIT
    : > "$WORK/cmd_line.txt"
    if [ -f "$CMD_LINE" ]; then
        cp -- "$CMD_LINE" "$WORK/cmd_line.txt"
    fi
    saved_options "$WORK/cmd_line.txt" > "$WORK/options"
    mapfile -d '' OPTIONS < "$WORK/options"
    if [[ " ${OPTIONS[*]} " != *" -Dpython.install_env="* ]]; then
        OPTIONS+=("-Dpython.install_env=venv")
    fi
    printf 'Option: %s\n' "${OPTIONS[@]}"

    if [[ " $* " == *" --reconfigure "* ]]; then
        sudo rm -rf -- "$BUILD_DIR"
    fi
    if ! meson setup "$BUILD_DIR" "$(dirname -- "$BUILD_DIR")" "${OPTIONS[@]}"; then
        # Kept for the next run, which would take the defaults without them
        mkdir -p "$(dirname -- "$CMD_LINE")"
        cp -- "$WORK/cmd_line.txt" "$CMD_LINE"
        echo "The build directory could not be set up. Its options are kept in" >&2
        echo "$CMD_LINE, correct them there if one of them is the reason." >&2
        exit 1
    fi
fi

meson compile -C "$BUILD_DIR"
meson compile -C "$BUILD_DIR" configure-additional-howdy-authentication-override
meson compile -C "$BUILD_DIR" configure-additional-install
meson compile -C "$BUILD_DIR" configure-additional-bin

# Reads a path from the generated paths.py (key as in paths.py), which only
# exists when the Python part is built, otherwise from paths.hh (constant name)
read_path() {
    if [ -f "$BUILD_DIR/howdy/paths.hh" ]; then
        sed -n "/$2/s/.*= \"\([^\"]*\)\".*/\1/p" "$BUILD_DIR/howdy/paths.hh"
    else
        sed -n "/$1/s/.*PurePath(\"\([^\"]*\)\").*/\1/p" "$BUILD_DIR/howdy/paths.py"
    fi
}

MODELS_DIR="$(read_path user_models_dir USER_MODELS_DIR)"
# Empty without -Dmodels_key_file: the models are not encrypted
MODELS_KEY_FILE="$(read_path models_key_file MODELS_KEY_FILE)"
PYTHON_PATH="$(read_path python_path PYTHON_PATH)"
PYTHON_SITE_PACKAGES=""
if [ -n "$PYTHON_PATH" ] && [ -x "$PYTHON_PATH" ]; then
    PYTHON_SITE_PACKAGES="$("$PYTHON_PATH" -I -c 'import sysconfig; print(sysconfig.get_path("purelib"))')"
fi

# Seals the key (64 hex digits on stdin, never as an argument) to the TPM
seal_models_key() {
    sudo mkdir -p "$(dirname "$MODELS_KEY_FILE")"
    python3 -I -c 'import sys; sys.stdout.buffer.write(bytes.fromhex(sys.stdin.read().strip()))' |
        sudo systemd-creds encrypt --with-key=tpm2 --name=models-key - "$MODELS_KEY_FILE"
    sudo chmod 0600 "$MODELS_KEY_FILE"
}

# Encrypts the face models that are still plain JSON. The key is only handed out to
# units, so the daemon runs as a transient one. Returns the status of the daemon.
# The command of the management daemon as the unit starts it (the C++ binary or the
# Python module), both take the same arguments. Fills MANAGEMENT_COMMAND.
management_command() {
    mapfile -d '' MANAGEMENT_COMMAND < <(python3 -I -c 'import shlex, sys; [sys.stdout.write(a + "\0") for a in shlex.split(sys.argv[1])]' \
        "$(sed -n 's/^ExecStart=//p' "$BUILD_DIR/howdy/howdy-management.service")")
    [ "${#MANAGEMENT_COMMAND[@]}" -gt 0 ]
}

encrypt_models() {
    management_command
    if ! command -v "${MANAGEMENT_COMMAND[0]}" > /dev/null; then
        echo "The management daemon is not installed yet, run ./install.sh first" >&2
        return 1
    fi
    sudo systemd-run --quiet --pipe --wait --collect \
        -p "LoadCredentialEncrypted=models-key:$MODELS_KEY_FILE" -p UMask=0077 -p LimitCORE=0 \
        "${MANAGEMENT_COMMAND[@]}" --encrypt-models
}

# Does the recovery key (64 hex digits in $RECOVERY_KEY) decrypt the encrypted models? A wrong
# key would be sealed without anybody noticing, until the models are needed. The C++ daemon
# of this build is used, the installed one may be older and not know the check.
verify_recovery_key() {
    management_command
    if [ "$(basename "${MANAGEMENT_COMMAND[0]}")" = howdy-management-daemon ]; then
        MANAGEMENT_COMMAND=("$BUILD_DIR/howdy/howdy-management-daemon")
    fi
    local status=0
    printf '%s\n' "$RECOVERY_KEY" | sudo "${MANAGEMENT_COMMAND[@]}" --verify-key || status=$?
    case "$status" in
        0) ;;
        3) echo "There is nothing to check the key against, the models are not encrypted yet" ;;
        2)
            # An installed Python daemon that does not know the check yet
            read -rp "The installed daemon can not check the key. Seal it anyway? [y/N] " answer
            [[ "$answer" =~ ^[yY]$ ]] || exit 1
            ;;
        *)
            echo "The recovery key does not decrypt the face models, it is not sealed. Is it the right one?" >&2
            exit 1
            ;;
    esac
}

# ./install.sh --encrypt-models only does that, without installing
if [[ " $* " == *" --encrypt-models "* ]]; then
    if [ -z "$MODELS_KEY_FILE" ] || ! sudo test -e "$MODELS_KEY_FILE"; then
        echo "There is no key credential, build with -Dmodels_key_file and run ./install.sh first" >&2
        exit 1
    fi
    encrypt_models
    exit $?
fi

# The key of the model encryption. A new key is only made if there is none: with a
# new key the encrypted models could not be read any more. The recovery key is the
# key itself, so the models survive a TPM or firmware change (--restore-models-key)
if [ -n "$MODELS_KEY_FILE" ]; then
    if [[ " $* " == *" --restore-models-key "* ]]; then
        read -rsp "Recovery key of the face models: " RECOVERY_KEY
        echo
        RECOVERY_KEY="$(printf '%s' "$RECOVERY_KEY" | tr -d ' -' | tr 'A-F' 'a-f')"
        if ! [[ "$RECOVERY_KEY" =~ ^[0-9a-f]{64}$ ]]; then
            echo "The recovery key must be 64 hex digits" >&2
            exit 1
        fi
        verify_recovery_key
        if sudo test -e "$MODELS_KEY_FILE"; then
            sudo mv "$MODELS_KEY_FILE" "$MODELS_KEY_FILE.old"
            echo "The old credential was kept as $MODELS_KEY_FILE.old"
        fi
        printf '%s' "$RECOVERY_KEY" | seal_models_key
        echo "The key is sealed to this TPM again"
    elif ! sudo test -e "$MODELS_KEY_FILE"; then
        RECOVERY_KEY="$(openssl rand -hex 32)"
        printf '%s' "$RECOVERY_KEY" | seal_models_key
        echo
        echo -e "\033[38;5;208mRecovery key of the face models, write it down and keep it away from this computer.\e[0m"
        echo "It is not stored anywhere and shown only now. Without it the models are lost"
        echo "after a TPM or firmware change. Restore with: ./install.sh --restore-models-key"
        echo
        echo "    $(printf '%s' "$RECOVERY_KEY" | sed -E 's/(.{8})/\1-/g; s/-$//')"
        echo
    fi
    unset RECOVERY_KEY
fi

sudo bash -c '
    set -e

    # The face models are only for root, also the directory: the PAM module
    # asks the daemon whether a user has a model, it does not look itself
    mkdir -p "$2"
    chmod 0700 "$2"

    if [ -n "$3" ]; then
        rm -rf -- "$3/howdy" "$3/howdy-gtk"
    fi

    # howdy-gtk is a link to howdy now, meson does not replace a regular file
    # (an older howdy-gtk program or script) with a link
    if [ -e /usr/local/bin/howdy-gtk ] && [ ! -L /usr/local/bin/howdy-gtk ]; then
        rm -f /usr/local/bin/howdy-gtk
    fi

    meson install -C "$1"

    systemctl daemon-reload

    systemctl enable howdy-authentication.service
    systemctl restart howdy-authentication.service

    if systemctl is-active --quiet howdy-management.service; then
        systemctl stop howdy-management.service
    fi

    systemctl --global enable howdy-auth-popup.service
' bash "$BUILD_DIR" "$MODELS_DIR" "$PYTHON_SITE_PACKAGES"

# A pam_howdy.so that an older installation has put into another directory is not
# removed: a PAM file that names its full path would go on loading it. It is reported,
# with the files that name it.
report_old_pam_modules() {
    local installed="$1" old real reported=" "
    for old in /usr/lib/security/pam_howdy.so /lib/security/pam_howdy.so /usr/lib64/security/pam_howdy.so \
        /usr/lib/*/security/pam_howdy.so /usr/local/lib/security/pam_howdy.so /usr/local/lib/*/security/pam_howdy.so; do
        # (-ef: /lib is a link to /usr/lib on most systems)
        if [ ! -e "$old" ] || [ "$old" -ef "$installed" ]; then
            continue
        fi
        real="$(realpath -- "$old")"
        if [[ "$reported" == *" $real "* ]]; then
            continue
        fi
        reported+="$real "
        echo
        echo -e "\033[38;5;208mAn older PAM module is left at $old, the new one is $installed\e[0m"
        echo "Run 'sudo pam-auth-update', install the additional PAM files again if you use them,"
        echo "and delete the old module when nothing names it any more. It is named in:"
        # With and without /usr, both lead to it
        grep -lsF -- "${real#/usr}" /etc/pam.d/* || echo "(no file in /etc/pam.d)"
    done
}

PAM_MODULE="$(meson introspect "$BUILD_DIR" --installed |
    python3 -I -c 'import json, sys; print(next((path for path in json.load(sys.stdin).values() if path.endswith("/pam_howdy.so")), ""))')"
if [ -n "$PAM_MODULE" ]; then
    report_old_pam_modules "$PAM_MODULE"
fi

# Models of the time before the encryption are encrypted now. A failure (for example a
# file that is not valid) is reported, but does not stop the installation
if [ -n "$MODELS_KEY_FILE" ]; then
    encrypt_models || echo -e "\033[38;5;208mNot all face models could be encrypted, see above\e[0m"
fi

systemctl --user daemon-reload
systemctl --user restart howdy-auth-popup.service

DLIB_DIR="$(read_path dlib_data_dir DLIB_DATA_DIR)"
DLIB_MISSING=false
for model in \
    dlib_face_recognition_resnet_model_v1 \
    mmod_human_face_detector \
    shape_predictor_5_face_landmarks; do
    if [ ! -f "$DLIB_DIR/$model.dat" ]; then
        DLIB_MISSING=true
    fi
done
if $DLIB_MISSING; then
    echo
    echo -e "\033[38;5;208mPlease run 'sudo $DLIB_DIR/install.sh' if you plan on using dlib\e[0m"
fi

OPENVINO_DIR="$(read_path openvino_data_dir OPENVINO_DATA_DIR)"
OPENVINO_MISSING=false
for model in \
    face-detection-adas-0001 \
    face-reidentification-retail-0095 \
    anti-spoofing-480 \
    damo-flir \
    facial-landmarks-35-adas-0002; do
    if [ ! -f "$OPENVINO_DIR/$model.xml" ]; then
        OPENVINO_MISSING=true
    fi
done
if $OPENVINO_MISSING; then
    echo
    echo -e "\033[38;5;208mPlease run 'sudo $OPENVINO_DIR/install.sh' if you plan on using OpenVINO\e[0m"
fi
