Download and unpack `openvino` data files from the [openvino open model zoo](https://storage.openvinotoolkit.org/repositories/open_model_zoo), [vladimirwest's OpenVINO_Face_Anti-Spoofing](https://github.com/vladimirwest/OpenVINO_Face_Anti-Spoofing) and [damo flir from modelscope](https://modelscope.cn/models/iic/cv_manual_face-liveness_flir/summary)

```
shell
sudo ./install.sh
```

`install.sh.in` checks the SHA-256 of every downloaded file (`verify`) and stops
if one does not match, so a changed or replaced file is not installed. The
files of the open model zoo are versioned, the ones of the other two sources
come from a branch and could change. After a new version of a model the
checksum in `install.sh.in` has to be updated. The converted `damo-flir.xml`
and `damo-flir.bin` are not checked, they depend on the version of the
converter, the checksum is the one of the downloaded `model.onnx`.
