project('daily-agenda', 'c', version: '0.1.0', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
deps = [dependency('gio-2.0'), dependency('glib-2.0'), dependency('yyjson'), dependency('libcurl'), dependency('libical')]
inc = include_directories('include')
add_project_arguments('-DAGENDA_ZONEINFO="' + get_option('zoneinfo') + '"', language: 'c')
sources = files('src/common.c', 'src/time.c', 'src/calendar.c', 'src/process.c', 'src/feed.c', 'src/bar.c', 'src/state.c', 'src/cli.c')
executable('daily-agenda', sources, 'src/main.c', include_directories: inc, dependencies: deps, install: true)
if get_option('fixtures')
  executable('agenda-fixture', sources, 'tests/fixture.c', include_directories: inc, dependencies: deps)
  executable('agenda-limits', 'tests/limits.c')
  executable('agenda-pty', 'tests/pty.c', dependencies: meson.get_compiler('c').find_library('util', required: false))
  if host_machine.system() == 'linux'
    shared_library('agenda-network', 'tests/network.c', dependencies: meson.get_compiler('c').find_library('dl'), install: false)
  endif
  executable('agenda-checks', 'tests/check.c', dependencies: deps + [dependency('openssl')])
endif
