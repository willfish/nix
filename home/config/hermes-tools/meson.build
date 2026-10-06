project('hermes-tools', 'c', version: '0.1.0', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
link = []
if host_machine.system() == 'linux' and get_option('b_sanitize').contains('address')
  link = ['-no-pie']
endif
deps = [dependency('gio-2.0'), dependency('yyjson')]
executable('hermes-export', 'export.c', dependencies: deps, link_args: link, install: true)
managed_deps = deps + [dependency('yaml-0.1')]
core = static_library('hermes-managed', 'value.c', 'files.c', 'merge.c', 'declaration.c', dependencies: managed_deps)
mode = 0
foreach name : ['hermes-declaration', 'hermes-profile', 'hermes-telegram-route']
  executable(name, 'managed-main.c', link_with: core, dependencies: managed_deps, c_args: ['-DMODE=' + mode.to_string()], link_args: link, install: true)
  mode += 1
endforeach
