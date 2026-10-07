project('darwin-tools', 'c', version: '0.1.0', meson_version: '>=1.3', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
deps = [dependency('gio-2.0'), dependency('yyjson'), dependency('libgit2')]
link = []
if host_machine.system() == 'linux' and get_option('b_sanitize').contains('address')
  link = ['-no-pie']
endif
core = static_library('darwin-tools', 'common.c', 'health.c', 'preflight.c', 'deploy.c', dependencies: deps)
mode = 0
foreach name : ['darwin-health', 'darwin-preflight', 'darwin-deploy']
  executable(name, 'main.c', link_with: core, dependencies: deps, c_args: ['-DMODE=' + mode.to_string()], link_args: link, install: true)
  mode += 1
endforeach
if get_option('fixtures')
  executable('darwin-fixture', 'tests/fixture.c', link_with: core, dependencies: deps, include_directories: '.', link_args: link)
  executable('darwin-checks', 'tests/check.c', dependencies: deps, include_directories: '.', link_args: link)
endif
