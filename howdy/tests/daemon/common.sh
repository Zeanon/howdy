#!/bin/bash
# Shared setup for the tests of the C++ components, sourced by the run_*.sh
# scripts.
#
# Environment (all optional):
#   WORK_DIR           Directory for build output and test data (default: mktemp)
#   HOWDY_PYTHON       Python with dlib/opencv/openvino/dbus-next, used as
#                      reference and for helpers
#                      (default: /usr/local/lib/x86_64-linux-gnu/Howdy/.venv/bin/python3)
#   DLIB_DATA_DIR      dlib models (default: /usr/local/share/howdy/dlib-models)
#   OPENVINO_DATA_DIR  OpenVINO models (default: /usr/local/share/howdy/openvino-models)
#   OPENVINO_DIR       OpenVINO runtime with include/ and libs/ (e.g. the pip
#                      wheel dir). Without it pkg-config 'openvino' is tried,
#                      otherwise the tests are built without OpenVINO.
#   CXX                C++ compiler (default: g++)
#   SANITIZE=1         build with AddressSanitizer and UBSan

set -euo pipefail
export LC_ALL=C

TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$(cd "$TEST_DIR/../../src/cpp" && pwd)"
PY_SRC_DIR="$(cd "$TEST_DIR/../../src/python" && pwd)"
ASSETS_DIR="$(cd "$TEST_DIR/../../assets" && pwd)"
CONFIG_TEMPLATE="$ASSETS_DIR/config/config.ini"

WORK_DIR="${WORK_DIR:-$(mktemp -d -t howdy-daemon-tests.XXXXXX)}"
HOWDY_PYTHON="${HOWDY_PYTHON:-/usr/local/lib/x86_64-linux-gnu/Howdy/.venv/bin/python3}"
DLIB_DATA_DIR="${DLIB_DATA_DIR:-/usr/local/share/howdy/dlib-models}"
OPENVINO_DATA_DIR="${OPENVINO_DATA_DIR:-/usr/local/share/howdy/openvino-models}"
CXX="${CXX:-g++}"

IMG_DIR="$WORK_DIR/img"
OBJ_DIR="$WORK_DIR/obj"
T="$WORK_DIR/test"
mkdir -p "$IMG_DIR" "$OBJ_DIR" "$T/etc" "$T/models" "$T/cache" "$T/data/assets" "$T/gtkdata/assets" "$WORK_DIR/include" "$WORK_DIR/pyhowdy/howdy"
export T

echo "Work directory: $WORK_DIR"

# Substitute the meson placeholders of a template with test values
configure() {
	sed \
		-e "s|@version@|test|g" \
		-e "s|@config_dir@|$T/etc|g" \
		-e "s|@user_models_dir@|$T/models|g" \
		-e "s|@openvino_cache_dir@|$T/cache|g" \
		-e "s|@dlib_data_dir@|$DLIB_DATA_DIR|g" \
		-e "s|@openvino_data_dir@|$OPENVINO_DATA_DIR|g" \
		-e "s|@log_path@|$T/log|g" \
		-e "s|@data_dir@|$T/data|g" \
		-e "s|@gtk_data_dir@|$T/gtkdata|g" \
		-e "s|@authentication_bus@|org.boltgolt.howdy.authentication|g" \
		-e "s|@authentication_path@|/org/boltgolt/howdy/Authentication|g" \
		-e "s|@authentication_interface@|org.boltgolt.howdy.Authentication|g" \
		-e "s|@howdy_authentication_service_name@|howdy-authentication-test.service|g" \
		-e "s|@popup_path@|/org/boltgolt/howdy/AuthPopup|g" \
		-e "s|@popup_interface@|org.boltgolt.howdy.AuthPopup|g" \
		-e "s|@management_bus@|org.boltgolt.howdy.management|g" \
		-e "s|@management_path@|/org/boltgolt/howdy/Management|g" \
		-e "s|@management_interface@|org.boltgolt.howdy.Management|g" \
		"$1" > "$2"
}

configure "$SRC_DIR/common/paths.hh.in" "$WORK_DIR/include/paths.hh"
cp "$ASSETS_DIR/logo/logo.png" "$T/data/assets/logo.png"
# The glade files and logos of the window
GTK_ASSETS_DIR="$(cd "$TEST_DIR/../../../howdy-gtk" && pwd)"
cp "$GTK_ASSETS_DIR"/src/*.glade "$T/gtkdata/"
cp "$GTK_ASSETS_DIR"/assets/logo/logo.png "$T/gtkdata/assets/logo.png"
cp "$GTK_ASSETS_DIR"/assets/logo/logo_about.png "$T/gtkdata/assets/logo_about.png"

# Python reference package with the same paths
cp "$PY_SRC_DIR"/{services,face,enroll,common}/*.py "$WORK_DIR/pyhowdy/howdy/"
configure "$PY_SRC_DIR/common/paths.py" "$WORK_DIR/pyhowdy/howdy/paths.py"
touch "$WORK_DIR/pyhowdy/howdy/__init__.py"

# Third party headers as system includes, OpenCV warns a lot with C++20
system_includes() { sed -E 's/(^| )-I/\1-isystem /g'; }
read -r -a PKG_CFLAGS <<< "$(pkg-config --cflags dlib-1 opencv4 nlohmann_json INIReader libsystemd gtk+-3.0 | system_includes)"
read -r -a PKG_LIBS <<< "$(pkg-config --libs dlib-1 opencv4 INIReader libsystemd gtk+-3.0)"

SANITIZER_LINK=()
[ "${SANITIZE:-0}" = 1 ] && SANITIZER_LINK=(-fsanitize=address,undefined)
CXXFLAGS=(-std=c++20 -O2 -DHOWDY_WITH_GUI -I"$WORK_DIR/include" -I"$SRC_DIR/core" -I"$SRC_DIR/common" -I"$SRC_DIR/cli" -I"$SRC_DIR/gui")
[ "${SANITIZE:-0}" = 1 ] && CXXFLAGS+=(-g -fsanitize=address,undefined -fno-omit-frame-pointer)
OPENVINO_LIBS=()
HAS_OPENVINO=0
if [ -n "${OPENVINO_DIR:-}" ]; then
	OV_LIB="$(ls "$OPENVINO_DIR"/libs/libopenvino.so* | grep -E 'libopenvino\.so(\.[0-9]+)*$' | head -1)"
	CXXFLAGS+=(-DHOWDY_WITH_OPENVINO -isystem "$OPENVINO_DIR/include")
	OPENVINO_LIBS=("$OV_LIB" -Wl,-rpath,"$OPENVINO_DIR/libs")
	HAS_OPENVINO=1
elif pkg-config --exists openvino; then
	read -r -a OV_CFLAGS <<< "$(pkg-config --cflags openvino | system_includes)"
	read -r -a OPENVINO_LIBS <<< "$(pkg-config --libs openvino)"
	CXXFLAGS+=(-DHOWDY_WITH_OPENVINO "${OV_CFLAGS[@]}")
	HAS_OPENVINO=1
fi

# The components, as in meson.build
BASE_SOURCES=(
	"$SRC_DIR/common/util.cc"
	"$SRC_DIR/common/polkit.cc"
	"$SRC_DIR/common/management_client.cc"
)
CORE_SOURCES=(
	"$SRC_DIR/core/config.cc"
	"$SRC_DIR/core/dlib_backend.cc"
	"$SRC_DIR/core/liveness.cc"
	"$SRC_DIR/core/rubberstamps/rubberstamps.cc"
	"$SRC_DIR/core/rubberstamps/hotkey.cc"
	"$SRC_DIR/core/rubberstamps/nod.cc"
	"$SRC_DIR/core/rubberstamps/stamp.cc"
	"$SRC_DIR/core/snapshot.cc"
	"$SRC_DIR/core/user_models.cc"
	"$SRC_DIR/core/video_capture.cc"
)
if [ "$HAS_OPENVINO" = 1 ]; then
	CORE_SOURCES+=("$SRC_DIR/core/openvino_backend.cc")
else
	echo "OpenVINO not found, building without OpenVINO support"
fi
DAEMON_SOURCES=(
	"$SRC_DIR/daemon/main.cc"
	"$SRC_DIR/daemon/authenticator.cc"
	"$SRC_DIR/daemon/service.cc"
)
MANAGEMENT_SOURCES=("$SRC_DIR"/management/*.cc)
CLI_SOURCES=("$SRC_DIR"/cli/*.cc)
GUI_SOURCES=("$SRC_DIR"/gui/*.cc)
# What the window needs of the cli, the rest of the cli is not linked
GUI_CLI_SOURCES=("$SRC_DIR/cli/enroll_dialog.cc" "$SRC_DIR/cli/face_setup.cc")

object_path() {
	echo "$OBJ_DIR/$(echo "$1" | md5sum | cut -c1-8)_$(basename "$1").o"
}

# Compile the sources that are not up to date, in parallel
build_objects() {
	local jobs=0 failed=0 source out
	local pids=()
	for source in "$@"; do
		out="$(object_path "$source")"
		if [ -f "$out" ] && [ -z "$(find "$SRC_DIR" "$TEST_DIR" "$WORK_DIR/include" -newer "$out" \( -name '*.cc' -o -name '*.hh' \) -print -quit)" ]; then
			continue
		fi
		"$CXX" "${CXXFLAGS[@]}" "${PKG_CFLAGS[@]}" -c "$source" -o "$out" &
		pids+=($!)
		if (( ++jobs >= $(nproc) )); then
			wait -n || failed=1
			jobs=$((jobs - 1))
		fi
	done
	for pid in "${pids[@]}"; do
		wait "$pid" || failed=1
	done
	if [ "$failed" = 1 ]; then
		echo "Compilation failed"
		exit 1
	fi
}

# link <output> <sources...>
link() {
	local output="$1" objects=() source
	shift
	build_objects "$@"
	for source in "$@"; do
		objects+=("$(object_path "$source")")
	done
	"$CXX" "${SANITIZER_LINK[@]}" "${objects[@]}" -o "$output" "${OPENVINO_LIBS[@]}" "${PKG_LIBS[@]}" -lpthread
}

link_daemon() { link "$1" "${BASE_SOURCES[@]}" "${CORE_SOURCES[@]}" "${DAEMON_SOURCES[@]}"; }
link_management() { link "$1" "${BASE_SOURCES[@]}" "${MANAGEMENT_SOURCES[@]}"; }
link_cli() { link "$1" "${BASE_SOURCES[@]}" "${CORE_SOURCES[@]}" "${CLI_SOURCES[@]}" "${GUI_SOURCES[@]}"; }

"$TEST_DIR/fetch_images.sh" "$IMG_DIR"

# Videos that serve as cameras, made of the dlib example images:
#   still.mp4    two faces       single.mp4   exactly one face (what an enrollment needs)
#   blank.mp4    no face         genuine.dat  the model of the first face of the still
make_test_videos() {
	[ -f "$T/single.mp4" ] && return
	ffmpeg -loglevel error -y -loop 1 -i "$IMG_DIR/2008_001009.jpg" -t 3 -r 15 -pix_fmt yuv420p -vf scale=360:480 "$T/still.mp4"
	ffmpeg -loglevel error -y -f lavfi -i color=c=gray:s=360x480:d=3 -r 15 -pix_fmt yuv420p "$T/blank.mp4"
	"$HOWDY_PYTHON" -I "$TEST_DIR/parity.py" "$WORK_DIR/pyhowdy" hog "$IMG_DIR/2008_001009.jpg" > "$T/hog.json"
	"$HOWDY_PYTHON" -I "$TEST_DIR/mkmodel.py" "$T/hog.json" 2008_001009.jpg 0 "$T/genuine.dat"
	# The example image shows two people, the enrollment needs exactly one face
	"$HOWDY_PYTHON" -I "$TEST_DIR/crop_face.py" "$T/hog.json" "$IMG_DIR/2008_001009.jpg" "$T/single.jpg"
	ffmpeg -loglevel error -y -loop 1 -i "$T/single.jpg" -t 3 -r 15 -pix_fmt yuv420p "$T/single.mp4"
}
