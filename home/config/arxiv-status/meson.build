project('arxiv-status', 'c', version: '0.1.0', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
status = executable('hypr-arxiv-status', 'main.c', dependencies: dependency('yyjson'), install: true)
node = find_program('node')
test('arxiv-status', node, args: ['--experimental-strip-types', '--test', files('status.test.ts')], env: ['ARXIV_STATUS_BIN=' + status.full_path()])
