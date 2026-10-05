project('pi-voice-c', 'c', version: '0.2.0', default_options: ['c_std=c11', 'warning_level=2'])
cc = meson.get_compiler('c')
add_project_arguments('-D_POSIX_C_SOURCE=200809L', '-D_DEFAULT_SOURCE', language: 'c')

yyjson = dependency('yyjson')
libcurl = dependency('libcurl')
pcre2 = dependency('libpcre2-8')
threads = dependency('threads')
cairo = dependency('cairo')
pango = dependency('pangocairo')
glib = dependency('glib-2.0')
gtk = dependency('gtk4')
layer_shell = dependency('gtk4-layer-shell-0')
libm = cc.find_library('m', required: false)
core_deps = [yyjson, libcurl, pcre2, threads, cairo, pango, glib, libm]
included = include_directories('include')

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
  'src/ipc.c',
  'src/runtime_adapters.c',
  'src/harness.c',
  'src/controller.c',
  'src/controller_json.c',
  'src/controller_delivery.c',
  'src/controller_record.c',
  'src/controller_monitor.c',
  'src/selection.c',
  'src/presentation.c',
  'src/menu.c',
  'src/conversation.c',
  'src/osd_main.c',
  include_directories: included,
  dependencies: core_deps,
  c_args: ['-DVOICE_MENU_NO_MAIN', '-DVOICE_OSD_NO_MAIN'],
  install: true,
)

voice_dep = declare_dependency(
  link_with: libpivoice,
  include_directories: included,
  dependencies: core_deps,
)

voice_cli = executable('pi-voice-c', 'src/main.c', dependencies: voice_dep, install: true)
executable('voice-menu-c', 'src/menu.c', dependencies: voice_dep, install: true)
executable('personaplex-open-c', 'src/conversation.c',
  c_args: ['-DVOICE_CONVERSATION_MAIN'], dependencies: voice_dep, install: true)
executable('pi-voice-osd-c', 'src/osd_main.c',
  dependencies: [voice_dep, gtk, layer_shell], install: true)
executable('voice-controller-fixture', 'tests/pi_voice_fixture.c',
  dependencies: voice_dep, install: true, install_dir: get_option('libexecdir'))

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
  'tests/test_ipc.c',
  'tests/test_runtime_adapters.c',
  'tests/test_harness.c',
  'tests/test_controller.c',
  'tests/test_main_adapters.c',
  'tests/test_presentation.c',
  'tests/test_menu.c',
  'tests/test_conversation.c',
  'tests/test_osd_main.c',
  dependencies: voice_dep,
)
test('voice', test_exe, timeout: 120)
python = find_program('python3')
test('daemon', python, args: [files('tests/daemon_smoke.py'), voice_cli], timeout: 60)
test('native-journey', python, args: [files('tests/native_journey.py'), voice_cli], timeout: 60)
