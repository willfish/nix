project('herdr-notification-focus', 'c', version: '0.1.0', meson_version: '>=1.1', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
deps = [dependency('gio-2.0'), dependency('yyjson'), meson.get_compiler('c').find_library('m')]
inc = include_directories('include')
sources = files('src/json.c', 'src/resolve.c', 'src/cache.c', 'src/system.c', 'src/cli.c')
executable('herdr-notification-focus', sources, 'src/main.c', dependencies: deps, include_directories: inc, install: true)
if get_option('fixtures')
  executable('focus-fixture', sources, 'tests/fixture.c', dependencies: deps, include_directories: inc)
  executable('hyprfake', 'tests/hyprfake.c', dependencies: deps)
  executable('focus-peer', 'tests/peer.c', dependencies: deps)
  executable('focus-checks', 'tests/check.c', dependencies: deps)
endif
