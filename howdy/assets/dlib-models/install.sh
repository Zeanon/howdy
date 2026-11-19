#!/bin/bash

if [[ "$EUID" != 0 ]]; then
    echo "This script needs to be run as root."
    exit 1
fi

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

echo "Downloading 3 required data files..."
download \
	"https://github.com/davisking/dlib-models/raw/master/dlib_face_recognition_resnet_model_v1.dat.bz2" \
	"dlib_face_recognition_resnet_model_v1.dat.bz2"

download \
	"https://github.com/davisking/dlib-models/raw/master/mmod_human_face_detector.dat.bz2" \
	"mmod_human_face_detector.dat.bz2"

download \
	"https://github.com/davisking/dlib-models/raw/master/shape_predictor_5_face_landmarks.dat.bz2" \
	"shape_predictor_5_face_landmarks.dat.bz2"

# Uncompress the data files and delete the original archive
echo " "
echo "Unpacking..."
bzip2 -d -f *.bz2

chmod 644 *.dat
