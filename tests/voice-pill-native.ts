// Disposable headless Sway smoke test, never the live desktop.
// node tests/voice-pill-native.ts /path/to/pi-voice-osd /private/output
import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { mkdtemp, mkdir, readFile, readdir, rm, writeFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { promisify } from 'node:util';
import { delay, listen, start, until } from '../home/config/voice-c/tests/support.ts';

const execute = promisify(execFile);
const binary = resolve(process.argv[2]), out = resolve(process.argv[3]);
await mkdir(out, { recursive: true, mode: 0o700 });
const root = await mkdtemp('/tmp/voice-pill-native-');
const runtime = join(root, 'runtime');
await mkdir(join(runtime, 'pi-voice'), { recursive: true, mode: 0o700 });
const config = join(root, 'sway.conf');
await writeFile(config, 'output * mode 1280x800\noutput * bg #343c49 solid_color\nseat seat0 fallback true\n');
const env = {
  PATH: process.env.PATH, HOME: root, LANG: 'C.UTF-8',
  XDG_DATA_DIRS: process.env.XDG_DATA_DIRS,
  XDG_RUNTIME_DIR: runtime, XDG_STATE_HOME: join(root, 'state'),
  XDG_CONFIG_HOME: join(root, 'config'), XDG_CURRENT_DESKTOP: 'sway',
  WLR_BACKENDS: 'headless', WLR_RENDERER: 'pixman', WLR_LIBINPUT_NO_DEVICES: '1',
  GSK_RENDERER: 'cairo', GTK_A11Y: 'none', GIO_USE_VFS: 'local',
};
const gtk = join(root, 'config/gtk-4.0');
await mkdir(gtk, { recursive: true });
await writeFile(join(gtk, 'gtk.css'), 'window, .background { background-color: #ff00ff; }\n');
const busConfig = join(root, 'bus.conf');
env.DBUS_SESSION_BUS_ADDRESS = `unix:path=${runtime}/bus`;
await writeFile(busConfig, `<busconfig><type>session</type>
<listen>${env.DBUS_SESSION_BUS_ADDRESS}</listen><auth>EXTERNAL</auth>
<policy context="default"><allow send_destination="*"/>
<allow receive_sender="*"/><allow own="*"/></policy></busconfig>`);
let state = { ok: true, phase: 'idle' };
const server = await listen(join(runtime, 'pi-voice/control.sock'), () => state);
const cases = [
  ['idle', { phase: 'idle' }], ['starting', { phase: 'starting' }],
  ['listening', { phase: 'recording', recording_seconds: 12, input_level: 0.06 }],
  ['destination', { phase: 'recording', recording_seconds: 1, input_level: 0.03 }],
  ['muted', { phase: 'recording', recording_seconds: 12, microphone: { muted: true } }],
  ['clipping', { phase: 'recording', recording_seconds: 12, input_level: 1, microphone: { clipping: true } }],
  ['transcribing', { phase: 'transcribing' }],
  ['retained', { phase: 'idle', retained: true, retained_source: 'Notes' }],
  ['speaking', { phase: 'idle', speaking: true, audible: true }],
  ['ready', { phase: 'draft', draft: true }],
  ['edited', { phase: 'draft', draft: true, draft_edited: true, osd: true }],
  ['cleared', { phase: 'idle', draft_edited: true, osd: true }],
  ['before-loss', { phase: 'recording', recording_seconds: 15, input_level: 0.03 }],
  ['connection-lost', { ok: false }], ['hidden-again', { phase: 'idle' }],
];
let app, sway, bus;
try {
  bus = start('dbus-daemon', ['--nofork', '--config-file', busConfig], env);
  sway = start('sway', ['--unsupported-gpu', '-c', config], env);
  env.WAYLAND_DISPLAY = await until(async () => {
    assert.equal(sway.output.done, false, sway.output.stderr);
    return (await readdir(runtime)).find(name => name.startsWith('wayland-') && !name.endsWith('.lock'));
  }, 'headless Wayland socket', 5000);
  app = start(binary, [], { ...env, WAYLAND_DEBUG: 'client' });
  for (const [name, status] of cases) {
    state = { ok: true, recording_label: 'pi · Notes', session_label: 'Notes', ...status };
    await delay(name === 'clipping' ? 2000 : 1500);
    assert.equal(app.output.done, false, `App exited in ${name}: ${app.output.stderr}`);
    await execute('grim', [join(out, `${name}.png`)], { env, timeout: 5000 });
    if (name === 'retained') {
      const { stdout: ppm } = await execute('grim', ['-t', 'ppm', '-g', '430,18 420x104', '-'], {
        env, timeout: 5000, encoding: 'buffer', maxBuffer: 1024 * 1024,
      });
      const header = Buffer.from('P6\n420 104\n255\n');
      assert.deepEqual(ppm.subarray(0, header.length), header);
      const pixels = ppm.subarray(header.length);
      for (const [x, y] of [[0, 0], [419, 0], [0, 103], [419, 103]]) {
        const offset = (y * 420 + x) * 3;
        assert.deepEqual(pixels.subarray(offset, offset + 3), Buffer.from([52, 60, 73]), 'Theme painted transparent margin');
      }
    }
    if (['retained', 'speaking', 'hidden-again'].includes(name)) {
      const offset = app.output.stderr.length;
      await delay(500);
      assert.ok(!app.output.stderr.slice(offset).includes('.frame('), `Idle animation frames in ${name}`);
    }
    console.log('Captured', name);
  }
} finally {
  for (const process of [app, sway, bus]) {
    if (!process) continue;
    process.child.kill('SIGTERM');
    const timer = setTimeout(() => process.child.kill('SIGKILL'), 3000);
    await process.exited;
    clearTimeout(timer);
  }
  if (app) await writeFile(join(out, 'app.log'), app.output.stdout + app.output.stderr);
  if (sway) await writeFile(join(out, 'compositor.log'), sway.output.stdout + sway.output.stderr);
  await server.close();
  await rm(root, { recursive: true, force: true });
}
const log = await readFile(join(out, 'app.log'), 'utf8');
assert.deepEqual(log.split('\n').filter(line => /Traceback|CRITICAL|Error|WARNING/.test(line)), []);
const hidden = await readFile(join(out, 'idle.png'));
assert.deepEqual(await readFile(join(out, 'hidden-again.png')), hidden, 'Pill did not hide');
for (const [name] of cases.slice(1, -1)) {
  assert.equal((await readFile(join(out, `${name}.png`))).equals(hidden), ['edited', 'cleared'].includes(name), `Wrong visibility: ${name}`);
}
assert.ok(log.includes('set_keyboard_interactivity(0)'), 'Keyboard focus not disabled');
const regions = new Map(), inputs = [];
for (const line of log.split('\n')) {
  const created = line.match(/create_region\(new id wl_region#(\d+)\)/);
  if (created) regions.set(created[1], false);
  const added = line.match(/wl_region#(\d+)\.add\(/);
  if (added) regions.set(added[1], true);
  const applied = line.match(/set_input_region\(wl_region#(\d+)\)/);
  if (applied) inputs.push(regions.get(applied[1]) === false);
}
assert.ok(inputs.length && inputs.every(Boolean), 'Surface has a nonempty input region');
console.log(`Native smoke: ${cases.length} states; transparent margins; idle frames; click-through; no keyboard capture or GTK warnings`);
