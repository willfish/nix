project('pi-voice-c', 'c', version: '0.1.0', default_options: ['c_std=c11', 'warning_level=2'])
cc = meson.get_compiler('c')
add_project_arguments('-D_POSIX_C_SOURCE=200809L', '-D_DEFAULT_SOURCE', language: 'c')

yyjson = dependency('yyjson')
libcurl = dependency('libcurl')
pcre2 = dependency('libpcre2-8')
threads = dependency('threads')
cairo = dependency('cairo')
pango = dependency('pangocairo')
glib = dependency('glib-2.0')
libm = cc.find_library('m', required: false)

libpivoice = library(
  'pivoice',
  'src/version.c',
  'src/spoken.c',
  'src/chunks.c',
  'src/text.c',
  'src/attachments.c',
  'src/engines.c',
  'src/capture.c',
  'src/devices.c',
  'src/audio.c',
  'src/labels.c',
  'src/osd.c',
  'src/pill_view.c',
  'src/pill_paint.c',
  include_directories: include_directories('include'),
  dependencies: [yyjson, libcurl, pcre2, threads, cairo, pango, glib, libm],
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
  'tests/test_capture.c',
  'tests/test_devices.c',
  'tests/test_audio.c',
  'tests/test_labels.c',
  'tests/test_osd.c',
  'tests/test_pill.c',
  link_with: libpivoice,
  include_directories: included,
  dependencies: [cairo, libm],
)
test('voice', test_exe)
