project('pi-voice-c', 'c', version: '0.2.0', meson_version: '>=1.1', default_options: ['c_std=c17', 'warning_level=3', 'werror=true'])
cc = meson.get_compiler('c')
add_project_arguments('-D_POSIX_C_SOURCE=200809L', '-D_DEFAULT_SOURCE', language: 'c')

yyjson = dependency('yyjson')
libcurl = dependency('libcurl')
pcre2 = dependency('libpcre2-8')
threads = dependency('threads')
cairo = dependency('cairo')
pango = dependency('pangocairo')
glib = dependency('glib-2.0')
# Keep pedantic diagnostics on owned code, not on upstream GTK headers.
gtk = dependency('gtk4', include_type: 'system')
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
if get_option('fixtures')
executable('voice-controller-fixture', 'tests/pi_voice_fixture.c', dependencies: voice_dep)
executable('voice-json-allocation', 'tests/json_allocation.c', dependencies: voice_dep)

test_exe = executable(
  'voice-tests',
  'tests/test_main.c',
  'tests/test_spoken.c',
  'tests/test_chunks.c',
  'tests/test_attachments.c',
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
  'tests/test_presentation.c',
  'tests/test_menu.c',
  'tests/test_conversation.c',
  'tests/test_osd_main.c',
  dependencies: voice_dep,
)
endif
