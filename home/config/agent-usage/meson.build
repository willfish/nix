project('agent-usage', 'c', version: '0.1.0', default_options: ['c_std=c11', 'warning_level=3', 'werror=true'])
add_project_arguments('-D_POSIX_C_SOURCE=200809L', '-D_DEFAULT_SOURCE', '-D_XOPEN_SOURCE=700', language: 'c')
cc = meson.get_compiler('c')
deps = [dependency('yyjson'), dependency('libcurl'), dependency('glib-2.0'), cc.find_library('m')]
core = static_library('usage', ['src/json.c', 'src/usage.c', 'src/io.c'],
  include_directories: 'include', dependencies: deps)
usage_dep = declare_dependency(link_with: core, include_directories: 'include', dependencies: deps)
foreach name : ['hypr-agent-status', 'omarchy-agent-usage-codex', 'omarchy-agent-usage-grok', 'omarchy-agent-usage-opencode']
  executable(name, 'src/main.c', dependencies: usage_dep, install: true)
endforeach
fixture = executable('usage-fixture', 'tests/fixture.c', dependencies: usage_dep)
node = find_program('node')
test('agent-usage', node, args: ['--experimental-strip-types', '--test', files('tests/usage.test.ts')],
  env: ['AGENT_USAGE_FIXTURE=' + fixture.full_path(), 'AGENT_USAGE_BIN_DIR=' + meson.current_build_dir()], timeout: 90)
