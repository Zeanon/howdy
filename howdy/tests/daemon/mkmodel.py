# mkmodel.py <parity json> <image name> <face index> <out>
import sys, json

d = json.load(open(sys.argv[1]))
img = [k for k in d if k.endswith(sys.argv[2])][0]
enc = d[img]["faces"][int(sys.argv[3])]["encoding"]
json.dump(
	[{"time": 0, "label": "test", "id": 0, "data": [enc]}], open(sys.argv[4], "w")
)
