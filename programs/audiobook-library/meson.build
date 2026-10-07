project('audiobook-library', 'c', version: '0.1.0', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
cc = meson.get_compiler('c')
deps = [dependency('gio-2.0'), dependency('yyjson'), cc.find_library('m')]
common = ['../lib/command-support/common.c', '../lib/repo-data/common.c']
inc = include_directories('../lib/command-support', '../pi-config/include')
executable('libation-inventory', ['libation.c'] + common, include_directories: inc, dependencies: deps, install: true)
foreach pair : [['qbittorrent-inventory', 'inventory.c'], ['source-target-duplicate-check', 'duplicates.c']]
  executable(pair[0], [pair[1], 'audio.c', '../pi-config/src/json.c'] + common,
    include_directories: inc, dependencies: deps, install: true)
endforeach
