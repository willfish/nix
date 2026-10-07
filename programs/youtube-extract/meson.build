project('youtube-extract', 'c', version: '0.1.0', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
cc = meson.get_compiler('c')
executable('youtube-extract', 'main.c', '../lib/command-support/common.c', '../lib/repo-data/common.c',
  include_directories: '../lib/command-support',
  dependencies: [dependency('gio-2.0'), dependency('yyjson'), cc.find_library('m')], install: true)
