# Parity harness, Python side. Usage: parity.py <pkgroot> <mode> <images...>
import sys
import json

sys.path.insert(0, sys.argv[1])

import cv2
import dlib

from howdy import paths_factory
from howdy.anti_spoof_core import TextureDetector

mode = sys.argv[2]
images = sys.argv[3:]

if mode in ("hog", "cnn"):
	detector = (
		dlib.cnn_face_detection_model_v1(paths_factory.mmod_human_face_detector_path())
		if mode == "cnn"
		else dlib.get_frontal_face_detector()
	)
	pose = dlib.shape_predictor(paths_factory.shape_predictor_5_face_landmarks_path())
	encoder = dlib.face_recognition_model_v1(
		paths_factory.dlib_face_recognition_resnet_model_v1_path()
	)
else:
	from howdy import openvino_face as ov

	detector = ov.FaceDetector(device="CPU")
	encoder = ov.FaceEncoder(device="CPU")
	spoof = ov.SpoofDetector(device="CPU")
	flir = ov.SpoofDetectorFlir(device="CPU")
	mm = ov.MicroMotionDetector(device="CPU")

out = {}
for path in images:
	frame = cv2.imread(path, cv2.IMREAD_COLOR)
	gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
	img = {}
	if mode == "openvino":
		img["spoof"] = spoof(frame)
		img["spoof_portrait"] = spoof(cv2.rotate(frame, cv2.ROTATE_90_CLOCKWISE))
	faces = []
	for fl in detector(gray, 1):
		if mode == "cnn":
			fl = fl.rect
		f = {"rect": [fl.left(), fl.top(), fl.right(), fl.bottom()]}
		t = TextureDetector()
		t.init(1)
		f["texture"] = t(gray, fl)
		if mode == "openvino":
			f["landmarks"] = mm._get_landmarks(frame, fl)[:5].tolist()
			e = encoder.encode(frame, fl)
			f["encoding"] = None if e is None else [float(x) for x in e]
			f["flir"] = flir(frame, fl)
		else:
			shape = pose(frame, fl)
			f["landmarks"] = [
				[shape.part(i).x, shape.part(i).y] for i in range(shape.num_parts)
			]
			f["encoding"] = list(encoder.compute_face_descriptor(frame, shape, 1))
		faces.append(f)
	img["faces"] = faces
	out[path] = img
print(json.dumps(out))
