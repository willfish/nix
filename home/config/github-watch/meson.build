project('github-watch', 'c', version: '0.1.0', meson_version: '>=1.1', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
deps = [dependency('gio-2.0'), dependency('yyjson')]
core = static_library('watch', ['src/watch.c', 'src/process.c'], include_directories: 'include', dependencies: deps)
watch_dep = declare_dependency(link_with: core, include_directories: 'include', dependencies: deps)
executable('github-notification-watch', 'src/main.c', dependencies: watch_dep, install: true)
if get_option('fixtures')
  executable('watch-fixture', 'tests/fixture.c', dependencies: watch_dep)
  executable('watch-fake', 'tests/fake.c', dependencies: deps)
  executable('watch-checks', 'tests/check.c', dependencies: deps)
endif
