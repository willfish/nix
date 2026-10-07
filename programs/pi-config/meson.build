project('pi-config', 'c', version: '0.1.0', meson_version: '>=1.3', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
glib = dependency('glib-2.0')
inc = include_directories('include')
link = []
if host_machine.system() == 'linux' and get_option('b_sanitize').contains('address')
  link = ['-no-pie']
endif
foreach kind : ['settings', 'auth']
  executable('pi-merge-' + kind, 'src/main.c', 'src/merge.c', 'src/io.c', 'src/json.c', dependencies: glib, include_directories: inc, c_args: kind == 'auth' ? ['-DAUTH_MERGER'] : [], link_args: link, install: true)
endforeach
executable('pi-capture-bus-host', 'src/bus-host.c', dependencies: glib, include_directories: inc, link_args: link, install: true)
if get_option('fixtures')
  executable('pi-host-argv', 'tests/host-argv.c', dependencies: glib, link_args: link)
  shared_module('pi-config-failures', 'tests/failures.c', dependencies: meson.get_compiler('c').find_library('dl'), name_prefix: '')
  executable('pi-config-check', 'tests/check.c', 'src/json.c', dependencies: glib, include_directories: inc, link_args: link)
endif
