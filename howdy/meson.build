if meson.is_subproject()
project('howdy', 'cpp', license: 'MIT', version: '3.0.1', meson_version: '>= 1.1.0', default_options: ['cpp_std=c++20'])
endif


datadir = get_option('prefix') / get_option('datadir') / 'howdy'
py_conf = configuration_data(paths_dict)
py_conf.set('data_dir', datadir)

howdy_version = '3.0.1'

# Which Python components are installed. Only the Python files that these
# components need are installed (see "Python sources" below)
need_py_auth = get_option('auth_daemon') == 'python'
need_py_management = get_option('management_daemon') == 'python'
need_py_popup = get_option('popup_service') == 'python'
# The Python window (howdy-gtk) imports the Python cli modules
need_py_cli = get_option('cli') == 'python' or get_option('gtk_app') == 'python'
need_py = need_py_auth or need_py_management or need_py_popup or need_py_cli

# The configured interpreter (by default the venv of the installation) only
# exists after it was created. The Python components need it, without them the
# Python that runs meson is used and the configured path is only passed on.
configured_python = paths_dict.get('python_path')
use_configured_python = need_py or import('fs').exists(configured_python)
if need_py and not import('fs').exists(configured_python)
	error('The Python components need the interpreter @0@, which does not exist. Create the venv before "meson setup" or set -Dpython_path.'.format(configured_python))
endif
if use_configured_python
	py = import('python').find_installation(configured_python)
	py_exec = py.full_path()
else
	py = import('python').find_installation()
	py_exec = configured_python
endif
py.dependency()

if need_py_auth
	authentication_exec = '"' + py_exec + '" -I -m howdy.howdy_authentication_daemon'
else
	authentication_exec = '"' + (installdir / 'howdy-authentication-daemon') + '"'
endif

if need_py_management
	management_exec = '"' + py_exec + '" -I -m howdy.howdy_management_daemon'
else
	management_exec = '"' + (installdir / 'howdy-management-daemon') + '"'
endif

if need_py_popup
	popup_exec = '"' + py_exec + '" -I -m howdy.howdy_popup_service'
else
	popup_exec = '"' + (installdir / 'howdy-auth-popup') + '"'
endif

# The authentication daemon saves the snapshots to the log path, which its unit makes read
# only otherwise (ProtectSystem=strict). Below /var/log systemd creates the directory.
if logpath.startswith('/var/log/') and logpath.substring(9).strip('/') != ''
	log_directory_setting = 'LogsDirectory=' + logpath.substring(9).strip('/') + '\nLogsDirectoryMode=0700'
else
	log_directory_setting = 'ReadWritePaths=-' + logpath
endif

# The management daemon writes the face models. Below /var/lib systemd creates the first directory (only for
# root, also if it exists with another mode) and makes it writable, the models directory in it is created by
# the daemon. For another path the unit may write to it if it exists.
models_state_dir = ''
if usermodelsdir.startswith('/var/lib/') and usermodelsdir.substring(9).strip('/') != ''
	models_state_dir = usermodelsdir.substring(9).strip('/').split('/')[0]
	models_directory_setting = 'StateDirectory=' + models_state_dir + '\nStateDirectoryMode=0700'
else
	models_directory_setting = 'ReadWritePaths=-' + usermodelsdir
endif

# The unit would not start without the credential, so the line is only there if the option is set
if modelskeyfile != ''
	models_key_setting = 'LoadCredentialEncrypted=models-key:' + modelskeyfile
else
	models_key_setting = ''
endif

service_conf_data = configuration_data({
	'models_key_setting': models_key_setting,
	'authentication_exec': authentication_exec,
	'management_exec': management_exec,
	'popup_exec': popup_exec,
	'python_path': py_exec,
	'openvino_cache_dir': openvinocachedir,
	'log_directory_setting': log_directory_setting,
	'user_models_dir': usermodelsdir,
	'models_directory_setting': models_directory_setting,
	'howdy_authentication_service_name': howdy_authentication_service_name,
	'authentication_bus': dbus_authentication_bus,
	'management_bus': dbus_management_bus,
	'howdy_management_service_name': howdy_management_service_name,
	'install_dir': installdir,
})
dbus_conf_data = configuration_data({
	'authentication_bus': dbus_authentication_bus,
	'popup_path': dbus_popup_path,
	'popup_interface': dbus_popup_interface,
	'authentication_path': dbus_authentication_path,
	'authentication_interface': dbus_authentication_interface,
	'management_bus': dbus_management_bus,
	'management_path': dbus_management_path,
	'management_interface': dbus_management_interface,
	'data_dir': datadir,
})


# <pam module>
pam_module_conf_data = configuration_data(paths_dict)
pam_module_conf_data.set('config_file_path', config_path)

# pam_unix is loaded by its full path, the directory depends on the distribution
pam_unix_path = get_option('pam_unix_path')
if pam_unix_path == ''
	pam_unix_path = '/usr/lib/security/pam_unix.so'
	foreach dir : ['/usr/lib/x86_64-linux-gnu/security', '/usr/lib64/security', '/usr/lib/aarch64-linux-gnu/security', '/usr/lib/security', '/lib/security']
		if import('fs').exists(dir / 'pam_unix.so')
			pam_unix_path = dir / 'pam_unix.so'
			break
		endif
	endforeach
	message('Using ' + pam_unix_path + ' as pam_unix, set pam_unix_path to change it')
endif
pam_module_conf_data.set('pam_unix_path', pam_unix_path)
pam_config_dir = get_option('prefix') / get_option('datadir') / 'pam-configs'
if (get_option('pam_config_dir') != '')
	pam_config_dir = get_option('pam_config_dir')
endif
foreach key : dbus_conf_data.keys()
	pam_module_conf_data.set(key, dbus_conf_data.get(key))
endforeach

inih_cpp = dependency('INIReader', fallback: ['inih', 'INIReader_dep'])
systemd_dep = dependency('libsystemd')
libevdev = dependency('libevdev')
libpam = meson.get_compiler('cpp').find_library('pam')
threads = dependency('threads')

# Translations
subdir('src/pam/po')

# Paths
paths_h = configure_file(
	input: 'src/pam/vars.hh',
	output: 'vars.hh',
	configuration: pam_module_conf_data
)

pamdir = get_option('pam_dir') != '' ? get_option('pam_dir') : join_paths('/', get_option('libdir'), 'security')

shared_library(
	'pam_howdy',
	'src/pam/main.cc',
	'src/pam/face_authenticator.cc',
	'src/pam/fingerprint_authenticator.cc',
	'src/pam/enter_device.cc',
	dependencies: [
		libpam,
		inih_cpp,
		threads,
		libevdev,
		systemd_dep,
	],
	link_depends: [
		paths_h,
	],
	# The module uses C++20 (jthread, std::format, starts_with), say it instead of relying on the default of the compiler
	override_options: ['cpp_std=c++20'],
	install: true,
	install_dir: pamdir,
	install_tag: 'pam_module',
	install_mode: 'rw-r--r--',
	name_prefix: ''
)
# </pam module>


# <popup service>
# The C++ popup service, the Python one is installed with the Python sources
build_popup = get_option('popup_service') == 'cpp'
build_cli = get_option('cli') == 'cpp'

# GTK is only needed by the C++ popup service and the C++ howdy program
# (cairo and the window)
if build_popup or build_cli
	gtk = dependency('gtk+-3.0', version: '>=3.24')
	cairo = dependency('cairo')
endif
if build_popup or (build_cli and get_option('gtk_app') == 'cpp')
	gio = dependency('gio-2.0')
endif

if build_popup
	popup_source = configure_file(
		input: 'src/cpp/popup/howdy_popup_service.cc',
		output: 'howdy_popup_service.cc',
		configuration: dbus_conf_data,
	)

	executable(
		'howdy-auth-popup',
		popup_source,
		dependencies: [
			gtk,
			gio,
			cairo,
		],
		install: true,
		install_dir: installdir,
	)
endif
# </popup service>


# <native components>
# The authentication daemon, the management daemon and the command line
# interface can be built in C++ or run from the Python sources
build_auth_daemon = get_option('auth_daemon') == 'cpp'
build_management_daemon = get_option('management_daemon') == 'cpp'
if get_option('gtk_app') == 'cpp' and not build_cli
	error('The C++ window (-Dgtk_app=cpp) is part of the C++ howdy program, it needs -Dcli=cpp')
endif

if build_auth_daemon or build_management_daemon or build_cli
	native_conf_data = configuration_data(paths_dict)
	foreach key : dbus_conf_data.keys()
		native_conf_data.set(key, dbus_conf_data.get(key))
	endforeach
	native_conf_data.set('version', howdy_version)
	# The window is part of the howdy program, its data is installed by howdy-gtk
	native_conf_data.set('gtk_data_dir', get_option('prefix') / get_option('datadir') / 'howdy-gtk')
	native_conf_data.set('howdy_authentication_service_name', howdy_authentication_service_name)

	native_paths_h = configure_file(
		input: 'src/cpp/common/paths.hh.in',
		output: 'paths.hh',
		configuration: native_conf_data,
	)

	nlohmann_json = dependency('nlohmann_json', include_type: 'system')
	openssl = dependency('openssl', version: '>=1.1.1')
	native_inc = include_directories('src/cpp/core', 'src/cpp/common')
	native_options = ['cpp_std=c++20']

	# The basics every component needs: user lookups, polkit and the client
	# of the management daemon
	base_lib = static_library(
		'howdy_base',
		[
			native_paths_h,
			'src/cpp/common/util.cc',
			'src/cpp/common/caller.cc',
			'src/cpp/common/trusted_file.cc',
			'src/cpp/common/model_crypto.cc',
			'src/cpp/common/polkit.cc',
			'src/cpp/common/management_client.cc',
		],
		include_directories: native_inc,
		dependencies: [systemd_dep, nlohmann_json, openssl],
		override_options: native_options,
	)
	base_dep = declare_dependency(
		link_with: base_lib,
		include_directories: native_inc,
		dependencies: [systemd_dep, nlohmann_json, openssl],
	)
endif

# The face recognition, used by the authentication daemon and the cli
native_rpath = ''
if build_auth_daemon or build_cli
	opencv = dependency(
		'opencv4',
		modules: ['opencv_core', 'opencv_imgproc', 'opencv_videoio', 'opencv_imgcodecs', 'opencv_highgui'],
		include_type: 'system',
	)
	dlib = dependency('dlib-1', include_type: 'system')

	# OpenVINO: a system wide installation (CMake config or pkg-config) is
	# preferred. Otherwise the runtime of the openvino pip wheel in the
	# configured Python (usually the Howdy venv) is used, which ships the
	# headers and the shared libraries.
	fs = import('fs')
	native_rpath = ''
	openvino = dependency('', required: false)
	if not get_option('openvino').disabled()
		message('Searching for the OpenVINO runtime')
		openvino = dependency(
			'OpenVINO',
			method: 'cmake',
			modules: ['openvino::runtime'],
			include_type: 'system',
			required: false,
		)
		if not openvino.found()
			openvino = dependency('openvino', method: 'pkg-config', include_type: 'system', required: false)
		endif

		if not openvino.found()
			# The package is only looked for, not imported: importing openvino starts its telemetry,
			# which creates ~/intel/openvino_ga_cid in the home of whoever runs meson
			openvino_probe = run_command(
				py, '-I', '-c',
				'\n'.join([
					'import glob, importlib.util, os, re',
					'spec = importlib.util.find_spec("openvino")',
					'if spec is None or not spec.submodule_search_locations: raise SystemExit(1)',
					'root = list(spec.submodule_search_locations)[0]',
					'libs = sorted(l for l in glob.glob(os.path.join(root, "libs", "libopenvino.so*"))',
					'			  if re.fullmatch(r"libopenvino\\.so(\\.\\d+)*", os.path.basename(l)))',
					'print(os.path.join(root, "include"))',
					'print(libs[0])',
				]),
				check: false,
			)
			openvino_paths = openvino_probe.stdout().strip().split('\n')
			if openvino_probe.returncode() == 0 and openvino_paths.length() == 2
				openvino_lib = openvino_paths[1]
				openvino_libdir = fs.parent(openvino_lib)
				message('Using the OpenVINO runtime of the Python wheel in @0@'.format(openvino_libdir))
				openvino = declare_dependency(
					compile_args: ['-isystem', openvino_paths[0]],
					link_args: [openvino_lib],
					version: run_command(py, '-I', '-c', 'import importlib.metadata; print(importlib.metadata.version("openvino").split("-")[0])', check: false).stdout().strip(),
				)
				# The wheel is not in the library search path, the daemon
				# needs to find it at runtime
				native_rpath = openvino_libdir
			endif
		endif

		if get_option('openvino').enabled() and not openvino.found()
			error('OpenVINO was requested but neither a system installation nor the openvino Python wheel in @0@ was found'.format(py_exec))
		endif
	endif

	core_sources = [
		'src/cpp/core/config.cc',
		'src/cpp/core/dlib_backend.cc',
		'src/cpp/core/liveness.cc',
		'src/cpp/core/snapshot.cc',
		'src/cpp/core/rubberstamps/rubberstamps.cc',
		'src/cpp/core/rubberstamps/hotkey.cc',
		'src/cpp/core/rubberstamps/nod.cc',
		'src/cpp/core/rubberstamps/stamp.cc',
		'src/cpp/core/user_models.cc',
		'src/cpp/core/video_capture.cc',
	]
	core_deps = [base_dep, opencv, dlib, inih_cpp, libevdev]
	core_args = []

	if openvino.found()
		core_sources += 'src/cpp/core/openvino_backend.cc'
		core_deps += openvino
		core_args += '-DHOWDY_WITH_OPENVINO'
	endif

	core_lib = static_library(
		'howdy_core',
		core_sources,
		include_directories: native_inc,
		dependencies: core_deps,
		cpp_args: core_args,
		override_options: native_options,
	)
	core_dep = declare_dependency(
		link_with: core_lib,
		dependencies: core_deps,
		compile_args: core_args,
	)
endif

if build_auth_daemon
	executable(
		'howdy-authentication-daemon',
		[
			'src/cpp/daemon/main.cc',
			'src/cpp/daemon/authenticator.cc',
			'src/cpp/daemon/service.cc',
		],
		include_directories: native_inc,
		dependencies: [core_dep, threads],
		override_options: native_options,
		build_rpath: native_rpath,
		install_rpath: native_rpath,
		install: true,
		install_dir: installdir,
		install_tag: 'auth_daemon',
	)
endif

if build_management_daemon
	executable(
		'howdy-management-daemon',
		[
			native_paths_h,
			'src/cpp/management/main.cc',
			'src/cpp/management/model_store.cc',
			'src/cpp/management/service.cc',
		],
		include_directories: native_inc,
		dependencies: [base_dep],
		override_options: native_options,
		install: true,
		install_dir: installdir,
		install_tag: 'management_daemon',
	)
endif

if build_cli
	cli_sources = [
		'src/cpp/cli/main.cc',
		'src/cpp/cli/common.cc',
		'src/cpp/cli/cmd_add.cc',
		'src/cpp/cli/cmd_config.cc',
		'src/cpp/cli/cmd_gui.cc',
		'src/cpp/cli/cmd_models.cc',
		'src/cpp/cli/cmd_snapshot.cc',
		'src/cpp/cli/cmd_test.cc',
		'src/cpp/cli/enroll_dialog.cc',
		'src/cpp/cli/face_setup.cc',
	]
	cli_deps = [core_dep, gtk, cairo, threads]
	cli_args = []

	# The configuration window is part of the same program: howdy gui, and
	# howdy-gtk is a link to it (see howdy-gtk/meson.build)
	if get_option('gtk_app') == 'cpp'
		cli_sources += [
			'src/cpp/gui/gdbus_client.cc',
			'src/cpp/gui/gui_main.cc',
			'src/cpp/gui/main_window.cc',
			'src/cpp/gui/model_enroller.cc',
			'src/cpp/gui/onboarding.cc',
			'src/cpp/gui/process.cc',
			'src/cpp/gui/video_preview.cc',
		]
		cli_deps += [gio]
		cli_args += '-DHOWDY_WITH_GUI'
	endif

	executable(
		'howdy',
		cli_sources,
		include_directories: [native_inc, include_directories('src/cpp/cli', 'src/cpp/gui')],
		dependencies: cli_deps,
		cpp_args: cli_args,
		override_options: native_options,
		build_rpath: native_rpath,
		install_rpath: native_rpath,
		install: true,
		install_dir: get_option('bindir'),
		install_tag: 'bin',
	)
endif
# </native components>


if get_option('install_pam_config')
	# pamdir is inherited from the pam subproject
	pam_config = configure_file(
		input: 'assets/pam-config/howdy.in',
		output: 'pam-config',
		configuration: {'pamdir': pamdir}
	)
	install_data(
		pam_config,
		install_dir: pam_config_dir,
		install_mode: 'rwxr-xr-x',
		install_tag: 'pam',
		rename: 'howdy',
	)
endif

if get_option('install_selinux_module')
	selinux_mod = custom_target(
		'howdy.mod',
		input: 'assets/selinux/howdy.te',
		output: 'howdy.mod',
		command: [find_program('checkmodule'), '-M', '-m', '-o', '@OUTPUT@', '@INPUT@'],
	)
	custom_target(
		'howdy.pp',
		input: selinux_mod,
		output: 'howdy.pp',
		command: [find_program('semodule_package'), '-o', '@OUTPUT@', '-m', '@INPUT@'],
		install: true,
		install_dir: get_option('prefix') / get_option('datadir') / 'selinux' / 'packages',
		install_tag: 'selinux',
	)
endif


if get_option('install_polkit_dropin')
	systemd = dependency('systemd', required: false)
	systemdunitdir = systemd.found() ? systemd.get_variable(pkgconfig: 'systemdsystemunitdir') : get_option('prefix') / 'lib' / 'systemd' / 'system'
	install_data(
		'assets/polkit/polkit-service.override',
		install_dir: systemdunitdir / 'polkit-agent-helper@.service.d',
		install_tag: 'polkit',
		rename: '10-howdy.conf',
	)
endif

additional_dir = join_paths(meson.current_source_dir(), '../additional')
kde_fingerprint = configure_file(
	input: 'assets/additional/bin/kde-fingerprint.in',
	output: 'kde-fingerprint',
	configuration: {'pamdir': pamdir}
)
polkit = configure_file(
	input: 'assets/additional/bin/polkit-1.in',
	output: 'polkit-1',
	configuration: {'pamdir': pamdir}
)
additional_bin = [
	kde_fingerprint,
	polkit,
]
run_target(
	'configure-additional-howdy-authentication-override',
	command: [
		'install',
		'-D',
		'-m', '0644',
		join_paths(
			meson.current_source_dir(),
			'assets/additional/howdy-authentication-service.override'
		),
		join_paths(
			additional_dir,
			'howdy-authentication-service.override'
		)
	],
)
run_target(
	'configure-additional-install',
	command: [
		'install',
		'-D',
		'-m', '0775',
		join_paths(
			meson.current_source_dir(),
			'assets/additional/install-additional.sh'
		),
		join_paths(
			additional_dir,
			'install-additional.sh'
		)
	],
)
run_target(
	'configure-additional-bin',
	command: [
		'install',
		'-D',
		'-m', '0755',
		'-t',
		join_paths(
			meson.current_source_dir(),
			'../additional/bin',
		),
		kde_fingerprint,
		polkit,
	],
)

# Python sources, only what the selected Python components import:
#   auth_daemon	   howdy_authentication_*, the capture and face code, rubberstamps
#   management_daemon howdy_management_daemon, cli/auth (polkit check)
#   popup_service	 howdy_popup_service
#   cli / gtk_app	 the cli package and the capture code it uses
if need_py
	need_py_capture = need_py_auth or need_py_cli
	need_py_cli_package = need_py_cli or need_py_management

	py_top = []
	py_cli = []
	py_recorders = []
	py_rubberstamps = []

	# Needed by every Python component
	py_top += [
		configure_file(
			input: 'src/python/common/paths.py',
			output: 'paths.py',
			configuration: py_conf,
		),
		'src/python/common/paths_factory.py'
	]

	if need_py_auth or need_py_popup or need_py_cli
		py_top += 'src/python/common/i18n.py'
	endif
	if need_py_auth or need_py_cli
		py_top += 'src/python/common/exit_codes.py'
	endif
	if need_py_cli
		py_top += 'src/python/common/model_limits.py'
	endif
	# The checks who calls a daemon, also used by the polkit check of the cli package
	if need_py_auth or need_py_management or need_py_cli
		py_top += 'src/python/common/caller.py'
	endif

	if need_py_auth
		py_top += [
			configure_file(
				input: 'src/python/services/howdy_authentication_daemon.py',
				output: 'howdy_authentication_daemon.py',
				configuration: dbus_conf_data,
			),
			configure_file(
				input: 'src/python/services/howdy_authentication_interface.py',
				output: 'howdy_authentication_interface.py',
				configuration: dbus_conf_data,
			),
			configure_file(
				input: 'src/python/services/howdy_popup_interface.py',
				output: 'howdy_popup_interface.py',
				configuration: dbus_conf_data,
			),
			'src/python/face/dlib_face.py',
		]
		py_rubberstamps += [
			'src/python/rubberstamps/__init__.py',
			'src/python/rubberstamps/hotkey.py',
			'src/python/rubberstamps/nod.py',
		]
	endif

	# Reading the models and writing them
	if need_py_auth or need_py_management
		py_top += 'src/python/common/file_util.py'
	endif

	if need_py_management
		py_top += configure_file(
			input: 'src/python/services/howdy_management_daemon.py',
			output: 'howdy_management_daemon.py',
			configuration: dbus_conf_data,
		)
	endif

	if need_py_popup
		py_top += configure_file(
			input: 'src/python/services/howdy_popup_service.py',
			output: 'howdy_popup_service.py',
			configuration: dbus_conf_data,
		)
	endif

	# Face capture, recognition and the camera recorders
	if need_py_capture
		py_top += [
			'src/python/face/anti_spoof_core.py',
			'src/python/enroll/enroll_capture.py',
			'src/python/enroll/enroll_helper.py',
			'src/python/face/openvino_face.py',
			'src/python/face/rotation.py',
		]
		py_recorders += [
			'src/python/recorders/__init__.py',
			'src/python/recorders/ffmpeg_reader.py',
			'src/python/recorders/pyv4l2_reader.py',
			'src/python/recorders/v4l2.py',
			'src/python/recorders/video_capture.py',
		]
	endif

	if need_py_cli_package
		py_cli += [
			'src/python/cli/__init__.py',
			configure_file(
				input: 'src/python/cli/auth.py',
				output: 'auth.py',
				configuration: dbus_conf_data,
			),
		]
	endif

	# The authentication daemon saves snapshots (save_failed, save_successful)
	if need_py_auth or need_py_cli
		py_top += 'src/python/common/snapshot.py'
	endif

	if need_py_cli
		py_cli += [
			'src/python/cli/__main__.py',
			configure_file(
				input: 'src/python/cli/add.py',
				output: 'add.py',
				configuration: dbus_conf_data,
			),
			configure_file(
				input: 'src/python/cli/clear.py',
				output: 'clear.py',
				configuration: dbus_conf_data,
			),
			configure_file(
				input: 'src/python/cli/config.py',
				output: 'config.py',
				configuration: service_conf_data,
			),
			'src/python/cli/disable.py',
			configure_file(
				input: 'src/python/cli/list.py',
				output: 'list.py',
				configuration: dbus_conf_data,
			),
			configure_file(
				input: 'src/python/cli/remove.py',
				output: 'remove.py',
				configuration: dbus_conf_data,
			),
			configure_file(
				input: 'src/python/cli/rename.py',
				output: 'rename.py',
				configuration: dbus_conf_data,
			),
			'src/python/cli/set.py',
			'src/python/cli/snap.py',
			configure_file(
				input: 'src/python/cli/test.py',
				output: 'test.py',
				configuration: dbus_conf_data,
			),
		]
	endif

	py_sources = {
		'': py_top,
		'cli': py_cli,
		'recorders': py_recorders,
		'rubberstamps': py_rubberstamps,
	}

	foreach py_subdir, py_subdir_sources : py_sources
		if py_subdir_sources.length() > 0
			py.install_sources(
				py_subdir_sources,
				subdir: 'howdy' / py_subdir,
				install_tag: 'py_sources',
			)
		endif
	endforeach
endif

install_data(
	'assets/logo/logo.png',
	'assets/logo/logo_popup.png',
	install_dir: join_paths(datadir, 'assets'),
	install_tag: 'meta'
)

autocomplete = configure_file(
	input: 'assets/autocomplete/howdy.in',
	output: 'autocomplete',
	configuration: configuration_data({ 'config_path': config_path })
)
install_data(
	autocomplete,
	install_dir: join_paths(get_option('prefix'), get_option('datadir'), 'bash-completion', 'completions'),
	install_mode: 'rwxr--r--',
	install_tag: 'bash_completion',
	rename: 'howdy',
)

fs = import('fs')
if get_option('install_config') and not fs.exists(config_path)
	install_data('assets/config/config.ini', install_dir: confdir, install_mode: 'rw-r--r--', install_tag: 'config')
endif

install_data(
	'assets/dlib-models/install.sh',
	'assets/dlib-models/Readme.md',
	install_mode: 'rwxr--r--',
	install_dir: dlibdatadir,
	install_tag: 'models'
)

openvino_install_bash = configure_file(
	input: 'assets/openvino-models/install.sh.in',
	output: 'install.sh',
	configuration: py_conf
)
install_data(
	openvino_install_bash,
	install_mode: 'rwxr--r--',
	install_dir: openvinodatadir,
	install_tag: 'models'
)

install_man('assets/man/howdy.1')

# if get_option('fetch_dlib_data')
#	 downloader = find_program('wget')
#	 bunzip2 = find_program('bunzip2')

#	 links = [ 
#		 'https://github.com/davisking/dlib-models/raw/master/dlib_face_recognition_resnet_model_v1.dat.bz2',
#		 'https://github.com/davisking/dlib-models/raw/master/mmod_human_face_detector.dat.bz2',
#		 'https://github.com/davisking/dlib-models/raw/master/shape_predictor_5_face_landmarks.dat.bz2'
#	 ]

#	 archived_model_files = [
#		 'dlib_face_recognition_resnet_model_v1.dat.bz2',
#		 'shape_predictor_5_face_landmarks.dat.bz2',
#		 'mmod_human_face_detector.dat.bz2'
#	 ]

#	 download = run_command(
#		 'download',
#		 links,
#		 output: archived_model_files,
#		 command: [downloader, '-O', '@OUTPUT@', '@INPUT@']
#	 )

#	 model_files = [
#		 'dlib_face_recognition_resnet_model_v1.dat',
#		 'shape_predictor_5_face_landmarks.dat',
#		 'mmod_human_face_detector.dat'
#	 ]

#	 models = custom_target(
#		 'models',
#		 input: archived_model_files,
#		 output: model_files,
#		 command: [bunzip2, '-k', '@INPUT@'],
#	 )

#	 install_data(
#		 model_files,
#		 install_dir: join_paths(installdir, 'dlib_models'),
#	 )

# endif

if get_option('cli') == 'python'
	bin = configure_file(
		input: 'assets/bin/howdy.in',
		output: 'howdy',
		configuration: service_conf_data
	)
	install_data(
	  bin,
	  install_mode: 'rwxr-xr-x',
	  install_dir: get_option('bindir'),
	  install_tag: 'bin',
	)
endif


# The directories the units may write to, only for root, there after the installation: the one of the face
# models (below /var/lib the first directory, systemd keeps it at 0700) and the cache of OpenVINO.
install_emptydir(models_state_dir != '' ? '/var/lib' / models_state_dir : usermodelsdir, install_mode: 'rwx------', install_tag: 'service')
install_emptydir(openvinocachedir, install_mode: 'rwx------', install_tag: 'service')

authentication_service = configure_file(
	input: 'assets/bin/howdy-authentication.service.in',
	output: howdy_authentication_service_name,
	configuration: service_conf_data
)
install_data(
	authentication_service,
	install_mode: 'rw-r--r--',
	install_dir: '/lib/systemd/system',
	install_tag: 'service',
)

management_service = configure_file(
	input: 'assets/bin/howdy-management.service.in',
	output: howdy_management_service_name,
	configuration: service_conf_data
)
install_data(
	management_service,
	install_mode: 'rw-r--r--',
	install_dir: '/lib/systemd/system',
	install_tag: 'service',
)

popup_service = configure_file(
	input: 'assets/bin/howdy-auth-popup.service.in',
	output: 'howdy-auth-popup.service',
	configuration: service_conf_data
)
install_data(
	popup_service,
	install_mode: 'rw-r--r--',
	install_dir: '/usr/lib/systemd/user',
	install_tag: 'service',
)

dbus_authentication_service = configure_file(
	input: 'assets/bin/org.boltgolt.howdy.authentication.service.in',
	output: 'org.boltgolt.howdy.authentication.service',
	configuration: service_conf_data
)
install_data(
	dbus_authentication_service,
	install_mode: 'rw-r--r--',
	install_dir: '/usr/share/dbus-1/system-services',
	install_tag: 'dbus-service',   
)

dbus_management_service = configure_file(
	input: 'assets/bin/org.boltgolt.howdy.management.service.in',
	output: 'org.boltgolt.howdy.management.service',
	configuration: service_conf_data
)
install_data(
	dbus_management_service,
	install_mode: 'rw-r--r--',
	install_dir: '/usr/share/dbus-1/system-services',
	install_tag: 'dbus-service',   
)

dbus_conf = configure_file(
	input: 'assets/bin/org.boltgolt.howdy.conf.in',
	output: 'org.boltgolt.howdy.conf',
	configuration: dbus_conf_data
)
install_data(
	dbus_conf,
	install_mode: 'rw-r--r--',
	install_dir: '/etc/dbus-1/system.d/',
	install_tag: 'dbus-conf',   
)

polkit_conf = configure_file(
	input: 'assets/polkit/com.github.boltgolt.howdy.policy.in',
	output: 'com.github.boltgolt.howdy.policy',
	configuration: dbus_conf_data
)
install_data(
	polkit_conf,
	install_dir: get_option('prefix') / get_option('datadir') / 'polkit-1' / 'actions',
	install_mode: 'rw-r--r--',
	install_tag: 'polkit',
)

if get_option('with_wakeup_hook')
	howdy_restart = configure_file(
		input: 'assets/bin/howdy-restart',
		output: 'howdy-restart',
		configuration: service_conf_data,
	)
	install_data(
		howdy_restart,
		install_mode: 'rwxr-xr-x',
		install_dir: '/lib/systemd/system-sleep/',
		install_tag: 'wakeup-hook',   
	)
endif
