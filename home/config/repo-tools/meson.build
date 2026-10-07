project('repo-tools', 'c', version: '0.1.0', meson_version: '>=1.1', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
deps = [dependency('glib-2.0'), dependency('yyjson')]
link = []
if host_machine.system() == 'linux' and get_option('b_sanitize').contains('address')
  link = ['-no-pie']
endif
executable('nix-storage-report', 'storage.c', 'common.c', dependencies: deps, link_args: link, install: true)
executable('audit-skills', 'audit.c', 'common.c', dependencies: deps, link_args: link, install: true)
executable('check-flake-lock-update', 'lock.c', 'common.c', dependencies: deps, link_args: link, install: true)
executable('import-omarchy-community', 'import.c', 'common.c', dependencies: deps + [dependency('libxml-2.0', version: '>=2.14')], link_args: link, install: true)
if get_option('fixtures')
  executable('repo-checks', 'tests/check.c', dependencies: deps, link_args: link, c_args: ['-DFIXTURE_SOURCE="' + meson.current_source_dir() + '"'])
endif
