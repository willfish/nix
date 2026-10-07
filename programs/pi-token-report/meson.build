project('pi-token-report', 'c', version: '0.1.0', meson_version: '>=1.1', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
cc = meson.get_compiler('c')
executable('pi-token-report', 'token-report.c', 'token-render.c', 'token-capture.c',
  '../lib/command-support/common.c', '../lib/repo-data/common.c',
  include_directories: '../lib/command-support',
  dependencies: [dependency('gio-2.0'), dependency('yyjson'), cc.find_library('m')],
  c_args: ['-DPROBE_PYTHON="' + get_option('probe_python') + '"'], install: true)
