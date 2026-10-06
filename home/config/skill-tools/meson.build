project('skill-tools', 'c', version: '0.1.0', meson_version: '>=1.3', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
cc = meson.get_compiler('c')
deps = [dependency('gio-2.0'), dependency('yyjson'), cc.find_library('m')]
link = []
if host_machine.system() == 'linux' and get_option('b_sanitize').contains('address')
  link = ['-no-pie']
endif
common = ['common.c', '../repo-tools/common.c']
foreach pair : [['contrast', 'contrast.c'], ['youtube-extract', 'youtube.c'], ['libation-inventory', 'libation.c']]
  executable(pair[0], [pair[1]] + common, dependencies: deps, link_args: link, install: true)
endforeach
foreach pair : [['qbittorrent-inventory', 'inventory.c'], ['source-target-duplicate-check', 'duplicates.c']]
  executable(pair[0], [pair[1], 'audio.c', '../pi-config/src/json.c'] + common,
    dependencies: deps, include_directories: '../pi-config/include', link_args: link, install: true)
endforeach
