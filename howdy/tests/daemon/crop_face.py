# Cut the first face found in a parity json (parity.py) out of its image,
# with a wide margin, as a square picture.
# Usage: crop_face.py <parity json> <image> <output image>
import sys
import json

import cv2

data = json.load(open(sys.argv[1]))
left, top, right, bottom = list(data.values())[0]["faces"][0]["rect"]
image = cv2.imread(sys.argv[2])

margin = int((right - left) * 0.8)
top = max(0, top - margin)
bottom = min(image.shape[0], bottom + margin)
left = max(0, left - margin)
right = min(image.shape[1], right + margin)
cv2.imwrite(sys.argv[3], cv2.resize(image[top:bottom, left:right], (360, 360)))
