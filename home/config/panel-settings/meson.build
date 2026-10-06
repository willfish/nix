project('panel-settings', 'c', version: '0.1.0', meson_version: '>=1.3', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
glib = dependency('glib-2.0')
inc = include_directories('include')
exe_link = []
if host_machine.system() == 'linux' and get_option('b_sanitize').contains('address')
  exe_link = ['-no-pie']
endif
foreach kind : ['calendar', 'weather']
  executable('hypr-' + kind + '-settings', 'src/main.c', 'src/json.c', dependencies: glib, include_directories: inc, c_args: ['-DPANEL_KIND="' + kind + '"'], link_args: exe_link, install: true)
endforeach
if get_option('fixtures')
  shared_module('panel-failures', 'tests/failures.c', dependencies: meson.get_compiler('c').find_library('dl'), name_prefix: '')
  executable('panel-argv', 'tests/argv.c', link_args: exe_link)
endif
