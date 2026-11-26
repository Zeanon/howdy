from pathlib import PurePath

# Define the absolute path to the config directory
config_dir = PurePath("@config_dir@")

# Define the absolute path to the DLib models data directory
dlib_data_dir = PurePath("@dlib_data_dir@")

# Define the absolute path to the OpenVINO models data directory
openvino_data_dir = PurePath("@openvino_data_dir@")

# Define the absolute path to the OpenVINO cache directory
openvino_cache_dir = PurePath("@openvino_cache_dir@")

# Define the absolute path to the Howdy user models directory
user_models_dir = PurePath("@user_models_dir@")

# Define path to any howdy logs
log_path = PurePath("@log_path@")

# Define the absolute path to the Howdy data directory
data_dir = PurePath("@data_dir@")

# Python interpreter configured for this build
python_path = PurePath("@python_path@")
