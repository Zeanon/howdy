#!/bin/bash

if [[ "$EUID" != 0 ]]; then
    echo "This script needs to be run as root."
    exit 1
fi

# The files belong next to this script, wherever it is started from
cd -- "$(dirname -- "${BASH_SOURCE[0]}")" || exit 1

if command -v wget >/dev/null 2>&1; then
    _PROGRESS_OPT=()
    if wget --help 2>&1 | grep -q -- '--show-progress'; then
        _PROGRESS_OPT=(-q --show-progress)
    fi

    download() {
        wget "${_PROGRESS_OPT[@]}" \
            --tries=6 \
            --timeout=30 \
            --max-redirect=1 \
            --output-document="$2" \
            "$1"
    }
elif command -v curl >/dev/null 2>&1; then
    download() {
        curl \
            --fail \
            --location \
            --retry 5 \
            --output "$2" \
            "$1"
    }
else
    echo "Error: Neither wget nor curl is installed." >&2
    exit 1
fi

# The files come from a branch of a repository and could change or be replaced
# without notice. Only use what has the expected SHA-256, the check runs before
# anything is unpacked.
verify() {
    if ! echo "$2  $1" | sha256sum --check --status; then
        echo "Error: $1 does not have the expected checksum ($2), it was not installed." >&2
        rm -f "$1"
        exit 1
    fi
}

echo "Downloading 3 required data files..."
download \
	"https://github.com/davisking/dlib-models/raw/master/dlib_face_recognition_resnet_model_v1.dat.bz2" \
	"dlib_face_recognition_resnet_model_v1.dat.bz2"
verify "dlib_face_recognition_resnet_model_v1.dat.bz2" \
	"abb1f61041e434465855ce81c2bd546e830d28bcbed8d27ffbe5bb408b11553a"

download \
	"https://github.com/davisking/dlib-models/raw/master/mmod_human_face_detector.dat.bz2" \
	"mmod_human_face_detector.dat.bz2"
verify "mmod_human_face_detector.dat.bz2" \
	"db9e9e40f092c118d5eb3e643935b216838170793559515541c56a2b50d9fc84"

download \
	"https://github.com/davisking/dlib-models/raw/master/shape_predictor_5_face_landmarks.dat.bz2" \
	"shape_predictor_5_face_landmarks.dat.bz2"
verify "shape_predictor_5_face_landmarks.dat.bz2" \
	"6e787bbebf5c9efdb793f6cd1f023230c4413306605f24f299f12869f95aa472"

# Uncompress the data files and delete the original archive
echo " "
echo "Unpacking..."
bzip2 -d -f *.bz2

chmod 644 *.dat
