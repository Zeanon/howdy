project('howdy', 'cpp', license: 'MIT', version: 'beta', meson_version: '>= 1.1.0')

dlibdatadir = get_option('dlib_data_dir') != '' ? get_option('dlib_data_dir') : join_paths(get_option('prefix'), get_option('datadir'), 'howdy/dlib-models')
openvinodatadir = get_option('openvino_data_dir') != '' ? get_option('openvino_data_dir') : join_paths(get_option('prefix'), get_option('datadir'), 'howdy/openvino-models')
openvinocachedir = get_option('openvino_cache_dir') != '' ? get_option('openvino_cache_dir') : '/var/cache/howdy/openvino'
confdir = get_option('config_dir') != '' ? get_option('config_dir') : join_paths(get_option('prefix'), get_option('sysconfdir'), 'howdy')
usermodelsdir = get_option('user_models_dir') != '' ? get_option('user_models_dir') : join_paths(confdir, 'models')
logpath = get_option('log_path')
installdir = get_option('install_dir') != '' ? get_option('install_dir') : join_paths(get_option('prefix'), get_option('libdir'), 'Howdy')
pythonpath = get_option('python_path') != '' ? get_option('python_path') : join_paths(installdir, '.venv/bin/python3')

config_path = join_paths(confdir, 'config.ini')


howdy_authentication_service_name = 'howdy-authentication.service'
howdy_management_service_name = 'howdy-management.service'

dbus_authentication_bus = 'org.boltgolt.howdy.authentication'
dbus_popup_path = '/org/boltgolt/howdy/AuthPopup'
dbus_popup_interface = 'org.boltgolt.howdy.AuthPopup'
dbus_authentication_path = '/org/boltgolt/howdy/Authentication'
dbus_authentication_interface = 'org.boltgolt.howdy.Authentication'

dbus_management_bus = 'org.boltgolt.howdy.management'
dbus_management_path = '/org/boltgolt/howdy/Management'
dbus_management_interface = 'org.boltgolt.howdy.Management'

paths_dict = {
    'config_dir': confdir,
    'dlib_data_dir': dlibdatadir,
    'openvino_data_dir': openvinodatadir,
    'openvino_cache_dir': openvinocachedir,
    'user_models_dir': usermodelsdir,
    'log_path': logpath,
    'python_path': pythonpath,
}

# howdy has to come first: its translations add project arguments, which has to
# happen before the first build target, and howdy-gtk builds one
subdir('howdy')
subdir('howdy-gtk')
subdir('howdy-cli')
