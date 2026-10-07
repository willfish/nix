project('voice-models', 'c', version: '0.1.0', meson_version: '>=1.1', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
deps = [dependency('glib-2.0'), dependency('libcurl')]
core = static_library('models', ['src/assets.c', 'src/download.c'], include_directories: 'include', dependencies: deps)
model_dep = declare_dependency(link_with: core, include_directories: 'include', dependencies: deps)
executable('voice-model-setup', 'src/main.c', dependencies: model_dep, install: true)
if get_option('fixtures')
  executable('model-fixture', 'tests/fixture.c', dependencies: model_dep)
  executable('model-http', 'tests/http.c', dependencies: dependency('openssl'))
  executable('model-checks', 'tests/check.c', dependencies: dependency('glib-2.0'),
    c_args: ['-DASSETS_JSON="' + join_paths(meson.current_source_dir(), 'tests/assets.json') + '"'])
endif
