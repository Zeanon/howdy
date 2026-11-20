# The manager script, meson sets the path of the config file
if get_option('install_howdy_cli')
    howdy_cli = configure_file(
        input: 'bin/howdy-manager.sh.in',
        output: 'howdy-cli',
        configuration: configuration_data({
            'config_file_path': config_path,
            'management_interface': dbus_management_interface,
        }),
    )

    install_data(
        howdy_cli,
        install_dir: get_option('prefix') / get_option('bindir'),
        install_mode: 'rwxr-xr-x',
        install_tag: 'cli_manager',
    )
endif
