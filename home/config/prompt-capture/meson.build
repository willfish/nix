project('prompt-capture-mitm', 'c', version: '0.1.0',
  default_options: ['c_std=c17', 'warning_level=2', 'werror=true'])
config = configuration_data()
config.set_quoted('PYTHON_EXECUTABLE', get_option('python_executable'))
config.set_quoted('SITE_PACKAGES', get_option('site_packages'))
configure_file(output: 'config.h', configuration: config)
executable('prompt-capture-mitm', 'main.c',
  dependencies: dependency('python3-embed'), install: true)
