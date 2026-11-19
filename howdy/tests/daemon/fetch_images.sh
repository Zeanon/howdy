#!/bin/bash
# Download the face example images of the dlib repository
# Usage: fetch_images.sh <target dir>
set -euo pipefail

dir="$1"
mkdir -p "$dir"
base=https://raw.githubusercontent.com/davisking/dlib/master/examples/faces
for f in 2007_007763.jpg 2008_001009.jpg 2008_001322.jpg 2008_002079.jpg bald_guys.jpg; do
	[ -s "$dir/$f" ] || curl -sfL -o "$dir/$f" "$base/$f"
done
