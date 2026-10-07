project('skill-tools', 'c', version: '0.1.0', meson_version: '>=1.3', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
cc = meson.get_compiler('c')
deps = [dependency('gio-2.0'), dependency('yyjson'), cc.find_library('m')]
link = []
if host_machine.system() == 'linux' and get_option('b_sanitize').contains('address')
  link = ['-no-pie']
endif
common = ['common.c', '../repo-tools/common.c']
executable('pi-token-report', ['token-report.c', 'token-render.c', 'token-capture.c'] + common,
  dependencies: deps, link_args: link, c_args: ['-DPROBE_PYTHON="' + get_option('probe_python') + '"'], install: true)
foreach pair : [['contrast', 'contrast.c'], ['youtube-extract', 'youtube.c'], ['libation-inventory', 'libation.c']]
  executable(pair[0], [pair[1]] + common, dependencies: deps, link_args: link, install: true)
endforeach
foreach pair : [['qbittorrent-inventory', 'inventory.c'], ['source-target-duplicate-check', 'duplicates.c']]
  executable(pair[0], [pair[1], 'audio.c', '../pi-config/src/json.c'] + common,
    dependencies: deps, include_directories: '../pi-config/include', link_args: link, install: true)
endforeach
if get_option('fixtures')
  baseline = join_paths(meson.current_source_dir(), '../llm/skills/pi-token-mitm/scripts/baseline-2026-09-12.json')
  executable('skill-tools-checks', ['tests/main.c', 'tests/run.c', 'tests/tools.c', 'tests/audio.c', 'tests/token.c'],
    dependencies: [dependency('yyjson'), cc.find_library('m')],
    c_args: ['-D_DEFAULT_SOURCE', '-DBASELINE="' + baseline + '"'])
  executable('ytdlp-stub', 'tests/ytdlp_stub.c', c_args: ['-D_DEFAULT_SOURCE', '-DSTUB_PARTIAL=0'])
  executable('ytdlp-partial', 'tests/ytdlp_stub.c', c_args: ['-D_DEFAULT_SOURCE', '-DSTUB_PARTIAL=1'])
  executable('capture-stub', 'tests/capture_stub.c', c_args: ['-D_DEFAULT_SOURCE'])
endif
