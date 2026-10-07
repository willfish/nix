project('opencode-adapt', 'c', version: '0.1.0', meson_version: '>=1.3', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
glib = dependency('glib-2.0')
inc = include_directories('include')
link = []
if host_machine.system() == 'linux' and get_option('b_sanitize').contains('address')
  link = ['-no-pie']
endif
executable('opencode-adapt-markdown', 'src/main.c', 'src/text.c', 'src/tree.c', dependencies: glib, include_directories: inc, link_args: link, install: true)
if get_option('fixtures')
  shared_module('opencode-adapt-failures', 'tests/failures.c', dependencies: meson.get_compiler('c').find_library('dl'), name_prefix: '')
  executable('opencode-adapt-checks', 'tests/check.c', dependencies: glib, include_directories: inc, link_args: link)
endif
