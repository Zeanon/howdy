# Compare the JSON output of parity.py and parity.cc
# Usage: compare.py <python json> <c++ json>
import sys
import json
import numpy as np

py = json.load(open(sys.argv[1]))
cc = json.load(open(sys.argv[2]))
worst = {"encoding": 0.0, "landmarks": 0.0, "texture": 0.0, "flir": 0.0, "spoof": 0.0}
ok = True
for img in py:
	p, c = py[img], cc[img]
	for key in ("spoof", "spoof_portrait"):
		if key in p:
			d = abs(p[key] - c[key])
			worst["spoof"] = max(worst["spoof"], d)
	if [f["rect"] for f in p["faces"]] != [f["rect"] for f in c["faces"]]:
		print(
			"RECT MISMATCH",
			img,
			[f["rect"] for f in p["faces"]],
			[f["rect"] for f in c["faces"]],
		)
		ok = False
		continue
	for fp, fc in zip(p["faces"], c["faces"]):
		worst["encoding"] = max(
			worst["encoding"],
			float(np.abs(np.array(fp["encoding"]) - np.array(fc["encoding"])).max()),
		)
		worst["landmarks"] = max(
			worst["landmarks"],
			float(np.abs(np.array(fp["landmarks"]) - np.array(fc["landmarks"])).max()),
		)
		worst["texture"] = max(
			worst["texture"],
			abs(fp["texture"] - fc["texture"]) / max(1, abs(fp["texture"])),
		)
		if "flir" in fp:
			worst["flir"] = max(worst["flir"], abs(fp["flir"] - fc["flir"]))
	print(img.split("/")[-1], "faces:", len(p["faces"]))

# dlib is expected to be bit identical, OpenVINO within float rounding
TOLERANCE = {
	"encoding": 1e-5,
	"landmarks": 1e-3,
	"texture": 1e-9,
	"flir": 1e-5,
	"spoof": 1e-5,
}
for key, limit in TOLERANCE.items():
	if worst[key] > limit:
		print(f"{key}: difference {worst[key]} exceeds {limit}")
		ok = False

print("max abs diff:", worst)
print("RESULT:", "OK" if ok else "MISMATCH")
sys.exit(0 if ok else 1)
