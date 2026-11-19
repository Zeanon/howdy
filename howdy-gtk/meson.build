if meson.is_subproject()
project('howdy-gtk', license: 'MIT', version: '1.0.0', meson_version: '>= 1.1.0')
endif

dlibdatadir = get_option('dlib_data_dir') != '' ? get_option('dlib_data_dir') : join_paths(get_option('prefix'), get_option('datadir'), 'dlib-data')
confdir = get_option('config_dir') != '' ? get_option('config_dir') : join_paths(get_option('prefix'), get_option('sysconfdir'), 'howdy')
usermodelsdir = get_option('user_models_dir') != '' ? get_option('user_models_dir') : join_paths(confdir, 'models')

datadir = get_option('prefix') / get_option('datadir') / 'howdy-gtk'
build_gtk_cpp = get_option('gtk_app') == 'cpp'

if not build_gtk_cpp
    py = import('python').find_installation(paths_dict.get('python_path'))
    py.dependency()

    py_conf = configuration_data(paths_dict)
    py_conf.set('data_dir', datadir)

    py_paths = configure_file(
        input: 'src/paths.py',
        output: 'paths.py',
        configuration: py_conf,
    )

    sources = files(
        'src/__main__.py',
        'src/i18n.py',
        'src/onboarding.py',
        'src/paths_factory.py',
        'src/tab_models.py',
        'src/tab_video.py',
    )

    py.install_sources(
        sources,
        py_paths,
        subdir: 'howdy-gtk',
        install_tag: 'py_sources',
    )
endif

logos = files(
    'assets/logo/logo.png',
    'assets/logo/logo_about.png',
)
install_data(
    logos,
    install_dir: join_paths(datadir, 'assets')
)

interface_files = files(
    'src/main.glade',
    'src/onboarding.glade',
)
install_data(interface_files, install_dir: datadir)

if build_gtk_cpp
    # The window is part of the howdy program (howdy gui), this is the command
    # that the desktop file starts
    install_symlink(
        'howdy-gtk',
        install_dir: get_option('prefix') / get_option('bindir'),
        pointing_to: 'howdy',
        install_tag: 'bin',
    )
else
    conf_data = configuration_data({ 'python_path': py.full_path() })

    bin_name = 'howdy-gtk'
    bin = configure_file(
        input: 'assets/bin/howdy-gtk.in',
        output: bin_name,
        configuration: conf_data
    )
    install_data(
      bin,
      install_mode: 'rwxr-xr-x',
      install_dir: get_option('prefix') / get_option('bindir'),
      install_tag: 'bin',
    )
endif

desktop_conf = configuration_data({ 'icon_path': join_paths(datadir, 'assets', 'logo.png') })
desktop_file = configure_file(
    input: 'assets/bin/howdy.desktop.in',
    output: 'Howdy.desktop',
    configuration: desktop_conf,
)
install_data(
    desktop_file,
    install_dir: get_option('prefix') / get_option('datadir') / 'applications',
    install_tag: 'desktop',
)
