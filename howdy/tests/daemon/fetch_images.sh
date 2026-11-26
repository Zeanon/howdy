#!/bin/bash
# Download the face example images of the dlib repository
# Usage: fetch_images.sh <target dir>
set -euo pipefail

dir="$1"
mkdir -p "$dir"
base=https://raw.githubusercontent.com/davisking/dlib/master/examples/faces
for f in 2007_007763.jpg 2008_001009.jpg 2008_001322.jpg 2008_002079.jpg bald_guys.jpg; do
	# Under its name only when it is complete: a download that is broken off must not count as done
	[ -s "$dir/$f" ] && continue
	curl -sfL -o "$dir/$f.part" "$base/$f"
	mv "$dir/$f.part" "$dir/$f"
done
