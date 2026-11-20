Download and unpack `dlib` data files from https://github.com/davisking/dlib-models repository:

```
shell
wget https://github.com/davisking/dlib-models/raw/master/dlib_face_recognition_resnet_model_v1.dat.bz2
wget https://github.com/davisking/dlib-models/raw/master/mmod_human_face_detector.dat.bz2
wget https://github.com/davisking/dlib-models/raw/master/shape_predictor_5_face_landmarks.dat.bz2
bunzip *bz2
```

`install.sh` checks the SHA-256 of every archive before it is unpacked and stops
if one does not match, so a changed or replaced file is not installed. After a
new version of a model the checksum in `install.sh` has to be updated, it is
the one of the `.bz2` file (`sha256sum <file>.dat.bz2`).
