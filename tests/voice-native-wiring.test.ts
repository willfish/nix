import assert from 'node:assert/strict';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import test from 'node:test';
import { invoke } from './helpers/voice-python-target.ts';

async function launcher(t, text?) {
  const root = await mkdtemp('/tmp/native-voice-wiring-');
  t.after(() => rm(root, { recursive: true, force: true }));
  const path = join(root, 'pi-voice');
  if (text !== undefined) await writeFile(path, text);
  return path;
}
test('voice fixture comes from the candidate, never the active profile', async t => {
  const path = await launcher(t, '#!/bin/sh\nexport PI_VOICE_COMMAND="$0"\nexec /nix/store/candidate-pi-voice-native/bin/pi-voice-c "$@"\n');
  assert.equal(invoke('locate', { launcher: path }).value, '/nix/store/candidate-pi-voice-native/libexec/voice-controller-fixture');
});
test('legacy launcher cannot silently test a different controller', async t => {
  const path = await launcher(t, 'exec python3 /old/voice_controller.py "$@"\n');
  const result = invoke('locate', { launcher: path });
  assert.equal(result.type, 'ValueError');
  assert.match(result.error, /not native/);
});
test('profiles without voice have no candidate fixture', async t => {
  assert.equal(invoke('locate', { launcher: await launcher(t) }).value, null);
});
test('only an exec line selects the native fixture', async t => {
  const path = await launcher(t, '# exec /nix/store/old/bin/pi-voice-c "$@"\necho "exec /nix/store/old/bin/pi-voice-c"\nexec /nix/store/current/bin/pi-voice-c "$@"\n');
  assert.equal(invoke('locate', { launcher: path }).value, '/nix/store/current/libexec/voice-controller-fixture');
});
test('behavior gate configures the candidate theme and voice fixtures', () => {
  const home = '/nix/store/fixture-home-manager-generation';
  const fixture = '/nix/store/native-voice/libexec/voice-controller-fixture';
  assert.deepEqual(invoke('configure', { home, fixture }).value, {
    PI_THEME_TEST_BIN: home + '/home-path/bin/pi', PI_VOICE_CONTROLLER_FIXTURE: fixture,
  });
});
