project('personaplex-tools', 'c', version: '0.1.0', meson_version: '>=1.1', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
deps = [dependency('glib-2.0'), dependency('libcurl'), dependency('libarchive'), dependency('yyjson')]
core = static_library('personaplex', ['src/common.c', 'src/http.c', 'src/archive.c', 'src/install.c', 'src/patch.c'], include_directories: 'include', dependencies: deps)
pp_dep = declare_dependency(link_with: core, include_directories: 'include', dependencies: deps)
executable('personaplex-models', 'src/main.c', dependencies: pp_dep, install: true)
executable('personaplex-patch', 'src/patch-main.c', dependencies: pp_dep, install: true)
if get_option('fixtures')
  executable('personaplex-fixture', 'tests/fixture.c', dependencies: pp_dep)
  executable('personaplex-checks', 'tests/check.c', dependencies: pp_dep)
endif
