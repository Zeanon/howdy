#!/bin/bash
# Download OpenVINO face detection and re-identification models for howdy
# These models run on Intel iGPU via OpenVINO for fast face unlock

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MODEL_DIR="/usr/share/howdy/openvino-models"
CACHE_DIR="/var/cache/howdy/openvino"
BASE_URL="https://storage.openvinotoolkit.org/repositories/open_model_zoo/2023.0/models_bin/1"

echo "Downloading OpenVINO face detection model (FP16)..."
curl -L --retry 3 -o "$SCRIPT_DIR/face-detection-adas-0001.xml" \
    "$BASE_URL/face-detection-adas-0001/FP16/face-detection-adas-0001.xml"
curl -L --retry 3 -o "$SCRIPT_DIR/face-detection-adas-0001.bin" \
    "$BASE_URL/face-detection-adas-0001/FP16/face-detection-adas-0001.bin"

echo "Downloading OpenVINO face re-identification model (FP16)..."
curl -L --retry 3 -o "$SCRIPT_DIR/face-reidentification-retail-0095.xml" \
    "$BASE_URL/face-reidentification-retail-0095/FP16/face-reidentification-retail-0095.xml"
curl -L --retry 3 -o "$SCRIPT_DIR/face-reidentification-retail-0095.bin" \
    "$BASE_URL/face-reidentification-retail-0095/FP16/face-reidentification-retail-0095.bin"

echo ""
echo "Installing models to $MODEL_DIR..."
mkdir -p "$MODEL_DIR"
cp "$SCRIPT_DIR"/*.xml "$SCRIPT_DIR"/*.bin "$MODEL_DIR/"

echo "Creating model cache directory at $CACHE_DIR..."
mkdir -p "$CACHE_DIR"
chmod 755 "$CACHE_DIR"

echo ""
echo "Done! OpenVINO models installed."
echo "The GPU model cache will be populated on first run (~10s), then loads in <500ms."
