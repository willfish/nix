project('omapager-tools', 'c', version: '0.1.0', meson_version: '>=1.3', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
glib = dependency('glib-2.0')
commands = ['omapager-prepare-shell', 'omapager-patch-icon', 'omapager-patch-herdr-focus']
exe_link = []
if host_machine.system() == 'linux' and get_option('b_sanitize').contains('address')
  exe_link = ['-no-pie']
endif
foreach name : commands
  executable(name, 'src/patch.c', dependencies: glib, c_args: ['-DTOOL_NAME="' + name + '"'], link_args: exe_link, install: true)
endforeach
if get_option('icons')
  python = dependency('python3')
  shared_module('icon_paths', 'src/icons.c', dependencies: python, name_prefix: '', install: true, install_dir: 'lib/omapager')
  if get_option('fixtures')
    executable('icon-fixture', 'tests/fixture.c', dependencies: [dependency('python3-embed'), glib], link_args: exe_link)
  endif
endif
