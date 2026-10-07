project('theme-menu', 'c', version: '0.1.0', meson_version: '>=1.1', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
deps = [dependency('gio-2.0'), dependency('yyjson')]
core = static_library('theme-menu', ['src/common.c', 'src/process.c', 'src/state.c', 'src/picker.c', 'src/desktop.c', 'src/cli.c'], include_directories: 'include', dependencies: deps)
theme_dep = declare_dependency(link_with: core, include_directories: 'include', dependencies: deps)
executable('theme-menu', 'src/main.c', dependencies: theme_dep, install: true)
if get_option('fixtures')
  executable('theme-menu-fixture', 'tests/fixture.c', dependencies: theme_dep)
  executable('theme-menu-fake', 'tests/fake.c', dependencies: deps)
  executable('theme-menu-check', 'tests/check.c', dependencies: deps)
endif
