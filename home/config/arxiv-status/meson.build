project('arxiv-status', 'c', version: '0.1.0', meson_version: '>=1.1', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
executable('hypr-arxiv-status', 'main.c', dependencies: dependency('yyjson'), install: true)
if get_option('fixtures')
  executable('arxiv-checks', 'tests/check.c', dependencies: dependency('yyjson'))
endif
