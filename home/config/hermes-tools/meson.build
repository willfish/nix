project('hermes-tools', 'c', version: '0.1.0', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
link = []
if host_machine.system() == 'linux' and get_option('b_sanitize').contains('address')
  link = ['-no-pie']
endif
executable('hermes-export', 'export.c', dependencies: [dependency('gio-2.0'), dependency('yyjson')], link_args: link, install: true)
