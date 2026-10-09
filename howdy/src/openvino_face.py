import os
import sys
import cv2
import dlib
import syslog
import numpy as np

MODEL_DIR = "/usr/share/howdy/openvino-models"
DET_XML = os.path.join(MODEL_DIR, "face-detection-adas-0001.xml")
REID_XML = os.path.join(MODEL_DIR, "face-reidentification-retail-0095.xml")
CACHE_DIR = "/var/cache/howdy/openvino"

_EXTRA_PATHS = [
    "/usr/local/lib64/python3.14/site-packages",
    "/usr/local/lib/python3.14/site-packages",
]

_core = None

def _get_core():
    global _core
    if _core is None:
        os.environ["OPENVINO_TELEMETRY_ENABLE"] = "0"
        for p in _EXTRA_PATHS:
            if p not in sys.path and os.path.isdir(p):
                sys.path.insert(0, p)
        from openvino import Core
        _core = Core()
        if os.path.isdir(CACHE_DIR):
            _core.set_property({"CACHE_DIR": CACHE_DIR})
    return _core


def is_available():
    return os.path.isfile(DET_XML) and os.path.isfile(REID_XML)


class FaceDetector:
    def __init__(self, device="GPU"):
        core = _get_core()
        model = core.read_model(DET_XML)
        self.compiled = core.compile_model(model, device)
        self.input_layer = self.compiled.input(0)
        self.output_layer = self.compiled.output(0)
        self.net_h = self.input_layer.shape[2]
        self.net_w = self.input_layer.shape[3]

    def __call__(self, frame, upsample=1):
        h, w = frame.shape[:2]
        if len(frame.shape) == 2:
            blob_frame = cv2.cvtColor(frame, cv2.COLOR_GRAY2BGR)
        else:
            blob_frame = frame
        blob = cv2.dnn.blobFromImage(blob_frame, size=(self.net_w, self.net_h), ddepth=cv2.CV_8U)
        results = self.compiled([blob])[self.output_layer]
        detections = []
        for det in results[0][0]:
            if det[2] > 0.5:
                x1 = max(0, int(det[3] * w))
                y1 = max(0, int(det[4] * h))
                x2 = min(w, int(det[5] * w))
                y2 = min(h, int(det[6] * h))
                detections.append(dlib.rectangle(x1, y1, x2, y2))
        return detections


class FaceEncoder:
    def __init__(self, device="GPU"):
        core = _get_core()
        model = core.read_model(REID_XML)
        self.compiled = core.compile_model(model, device)
        self.input_layer = self.compiled.input(0)
        self.output_layer = self.compiled.output(0)
        self.net_h = self.input_layer.shape[2]
        self.net_w = self.input_layer.shape[3]

    def encode(self, frame, face_rect):
        if len(frame.shape) == 2:
            frame = cv2.cvtColor(frame, cv2.COLOR_GRAY2BGR)
        x1 = max(0, face_rect.left())
        y1 = max(0, face_rect.top())
        x2 = min(frame.shape[1], face_rect.right())
        y2 = min(frame.shape[0], face_rect.bottom())
        face_crop = frame[y1:y2, x1:x2]
        if face_crop.size == 0:
            return None
        blob = cv2.dnn.blobFromImage(face_crop, size=(self.net_w, self.net_h), ddepth=cv2.CV_8U)
        result = self.compiled([blob])[self.output_layer]
        embedding = result.flatten()
        norm = np.linalg.norm(embedding)
        if norm > 0:
            embedding = embedding / norm
        return embedding
