project('login-theme-apply', 'c', version: '0.1.0', meson_version: '>=1.3', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
deps = [dependency('glib-2.0'), dependency('yyjson')]
inc = include_directories('include')
link = []
if host_machine.system() == 'linux' and get_option('b_sanitize').contains('address')
  link = ['-no-pie']
endif
executable('greeter-select', 'src/main.c', 'src/greeter.c', dependencies: deps, include_directories: inc, link_args: link, install: true)
if get_option('fixtures')
  executable('greeter-fixture', 'tests/fixture.c', 'src/greeter.c', dependencies: deps, include_directories: inc, link_args: link)
  shared_module('greeter-failures', 'tests/failures.c', dependencies: meson.get_compiler('c').find_library('dl'), name_prefix: '')
  executable('greeter-check', 'tests/check.c', dependencies: deps, include_directories: inc)
endif
