project('nix-storage-report', 'c', version: '0.1.0', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
executable('nix-storage-report', 'main.c', '../lib/repo-data/common.c',
  include_directories: '../lib/repo-data',
  dependencies: [dependency('glib-2.0'), dependency('yyjson')], install: true)
