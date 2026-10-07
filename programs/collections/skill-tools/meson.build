project('skill-tools-checks', 'c', version: '0.1.0', meson_version: '>=1.1', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
if get_option('fixtures')
  baseline = join_paths(meson.current_source_dir(), '../../../home/config/llm/skills/pi-token-mitm/scripts/baseline-2026-09-12.json')
  executable('skill-tools-checks', ['tests/main.c', 'tests/run.c', 'tests/tools.c', 'tests/audio.c', 'tests/token.c'],
    dependencies: [dependency('yyjson'), meson.get_compiler('c').find_library('m')],
    c_args: ['-D_DEFAULT_SOURCE', '-DBASELINE="' + baseline + '"'])
  executable('ytdlp-stub', 'tests/ytdlp_stub.c', c_args: ['-D_DEFAULT_SOURCE', '-DSTUB_PARTIAL=0'])
  executable('ytdlp-partial', 'tests/ytdlp_stub.c', c_args: ['-D_DEFAULT_SOURCE', '-DSTUB_PARTIAL=1'])
  executable('capture-stub', 'tests/capture_stub.c', c_args: ['-D_DEFAULT_SOURCE'])
endif
