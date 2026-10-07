project('wallpaper-cycle', 'c', version: '0.1.0', meson_version: '>=1.1', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
deps = [dependency('glib-2.0'), dependency('yyjson')]
executable('wallpaper-cycle', 'main.c', dependencies: deps, install: true)
if get_option('fixtures')
  executable('wallpaper-fake', 'tests/fake.c', dependencies: deps)
  executable('wallpaper-checks', 'tests/check.c', dependencies: deps)
endif
