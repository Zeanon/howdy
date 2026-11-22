#!/bin/bash
set -e

# ruff format --config "format.indent-style='tab'"

BUILD_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/build"

if [[ " $* " == *" --reconfigure "* ]]; then
    echo "######################################"
    echo "# Removing and rebuilding build dir #"
    echo "######################################"


    find "$(dirname -- "$BUILD_DIR")" \
        -type d -name build -prune -exec sudo rm -rf -- {} +
    meson setup "$BUILD_DIR" --python.install-env venv
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

# ./install.sh --encrypt-models only does that, without installing
if [[ " $* " == *" --encrypt-models "* ]]; then
    if [ -z "$MODELS_KEY_FILE" ] || ! sudo test -e "$MODELS_KEY_FILE"; then
        echo "There is no key credential, build with -Dmodels_key_file and run ./install.sh first" >&2
        exit 1
    fi
    encrypt_models
    exit $?
fi

sudo bash -c '
    set -e

    # The face models are only for root, also the directory: the PAM module
    # asks the daemon whether a user has a model, it does not look itself
    mkdir -p "$2"
    chmod 0700 "$2"

    rm -rf /usr/local/lib/x86_64-linux-gnu/Howdy/.venv/lib/python3.14/site-packages/howdy
    rm -rf /usr/local/lib/x86_64-linux-gnu/Howdy/.venv/lib/python3.14/site-packages/howdy-gtk

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
' bash "$BUILD_DIR" "$MODELS_DIR"

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
