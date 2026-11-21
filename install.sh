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
