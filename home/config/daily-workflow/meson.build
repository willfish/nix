project('daily-workflow', 'c', version: '0.1.0', meson_version: '>=1.1', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
executable('daily-workflow', 'main.c', dependencies: dependency('gio-2.0'), install: true)
if get_option('fixtures')
  executable('daily-pty', 'tests/pty.c', dependencies: dependency('gio-2.0'))
  executable('daily-fake', 'tests/fake.c')
  executable('daily-checks', 'tests/check.c', dependencies: dependency('gio-2.0'))
endif
