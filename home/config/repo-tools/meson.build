project('repo-tools', 'c', version: '0.1.0', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
deps = [dependency('glib-2.0'), dependency('yyjson')]
link = []
if host_machine.system() == 'linux' and get_option('b_sanitize').contains('address')
  link = ['-no-pie']
endif
executable('check-flake-lock-update', 'lock.c', 'common.c', dependencies: deps, link_args: link, install: true)
executable('import-omarchy-community', 'import.c', 'common.c', dependencies: deps + [dependency('libxml-2.0', version: '>=2.14')], link_args: link, install: true)
