project('pi-voice-c', 'c', version: '0.1.0', default_options: ['c_std=c11', 'warning_level=2'])
add_project_arguments('-D_POSIX_C_SOURCE=200809L', '-D_DEFAULT_SOURCE', language: 'c')

yyjson = dependency('yyjson')
libcurl = dependency('libcurl')
pcre2 = dependency('libpcre2-8')
threads = dependency('threads')

libpivoice = library(
  'pivoice',
  'src/version.c',
  'src/spoken.c',
  'src/chunks.c',
  'src/text.c',
  'src/attachments.c',
  'src/engines.c',
  include_directories: include_directories('include'),
  dependencies: [yyjson, libcurl, pcre2, threads],
  install: false,
)

included = include_directories('include')
test_exe = executable(
  'voice-tests',
  'tests/test_main.c',
  'tests/test_spoken.c',
  'tests/test_chunks.c',
  'tests/test_attachments.c',
  'tests/test_engines.c',
  link_with: libpivoice,
  include_directories: included,
)
test('voice', test_exe)
