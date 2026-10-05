import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, readlinkSync, rmSync, statSync, symlinkSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';

const binary = process.env.WALLPAPER_CYCLE_BIN;
assert.ok(binary, 'Set WALLPAPER_CYCLE_BIN to the candidate executable');
const legacy = process.env.WALLPAPER_CYCLE_LEGACY;
function fixture(t: any, names: any = ['a.png', 'b.png', 'c.png'], preferred: any = 'a.png') {
  const root = mkdtempSync(join(tmpdir(), 'wallpaper-cycle-'));
  t.after(() => rmSync(root, { recursive: true, force: true }));
  const state = join(root, 'state'), theme = join(root, 'theme'), bin = join(root, 'bin');
  for (const dir of [state, bin, join(theme, 'backgrounds')]) mkdirSync(dir, { recursive: true });
  for (const name of ['a.png', 'b.png', 'c.png', '雪.png', 'other.png']) writeFileSync(join(theme, 'backgrounds', name), name);
  writeFileSync(join(theme, 'theme.json'), JSON.stringify({ backgrounds: names, preferred }));
  const catalogue = join(root, 'catalogue.json');
  const manifest = { default: 'default-palette', palettes: { 'default-palette': { session: { dark: { 'wallpaper.png': 'nix-theme:rose' }, light: { 'wallpaper.png': 'nix-theme:light-rose' } } } } };
  writeFileSync(catalogue, JSON.stringify(manifest));
  const config = join(root, 'config.json'), log = join(root, 'log');
  const db: any = { state, theme, log, race: '', fail: '' };
  const script = `#!${process.execPath}
const fs = require('node:fs'), path = require('node:path');
const db = JSON.parse(fs.readFileSync(process.env.CYCLE_FAKE_DB, 'utf8'));
const name = path.basename(process.argv[1]), args = process.argv.slice(2);
fs.appendFileSync(db.log, JSON.stringify([name, ...args])+'\\n');
if (db.fail === name) process.exit(17);
if (name === 'nix') {
  try { fs.unlinkSync(args[2]); } catch (e) { if (e.code !== 'ENOENT') throw e; }
  fs.symlinkSync(db.theme, args[2]);
} else {
  fs.writeFileSync(args[1].slice(4), 'png:'+fs.readFileSync(args[0], 'utf8'));
  if (db.race === 'link') { fs.unlinkSync(path.join(db.state, 'wallpaper-source')); fs.symlinkSync(db.state, path.join(db.state, 'wallpaper-source')); }
  if (db.race === 'current') fs.writeFileSync(path.join(db.state, 'wallpaper-current'), 'other.png\\n');
}
`;
  for (const name of ['nix', 'magick']) writeFileSync(join(bin, name), script, { mode: 0o755 });
  const f = {
    root, state, theme, catalogue, manifest, db,
    link: join(state, 'wallpaper-source'), current: join(state, 'wallpaper-current'), live: join(state, 'active', 'wallpaper-live.png'),
    prepare() { if (!existsSync(f.link)) symlinkSync(theme, f.link); },
    run(direction = 'next', extra: string[] = []) {
      writeFileSync(config, JSON.stringify(db));
      const args = ['--state', state, '--catalogue', catalogue, '--flake', '/flake with spaces;no-shell', '--magick', join(bin, 'magick'), '--direction', direction, ...extra];
      const result = spawnSync(legacy ? 'python3' : binary!, legacy ? [legacy, ...args] : args, { encoding: 'utf8', env: { ...process.env, PATH: `${bin}:${process.env.PATH}`, CYCLE_FAKE_DB: config }, timeout: 5000 });
      assert.equal(result.signal, null, result.stderr);
      assert.equal(result.error, undefined);
      return result.status;
    },
    calls(): string[][] { return existsSync(log) ? readFileSync(log, 'utf8').trim().split('\n').map(line => JSON.parse(line)) : []; },
    selection(value: string) { writeFileSync(join(state, 'selection'), value); },
    source(value: any) { manifest.palettes['default-palette'].session.dark['wallpaper.png'] = value; writeFileSync(catalogue, JSON.stringify(manifest)); },
    oldLive() { mkdirSync(join(state, 'active'), { recursive: true }); writeFileSync(f.live, 'old image'); },
  };
  return f;
}

test('lazily builds exactly the selected theme, converts its next image and writes private current state', t => {
  const f = fixture(t);
  assert.equal(f.run(), 0);
  assert.deepEqual(f.calls(), [ ['nix', 'build', '--out-link', f.link, '/flake with spaces;no-shell#theme-rose'], ['magick', join(f.theme, 'backgrounds/b.png'), `PNG:${join(f.state, 'active/.wallpaper-live.png.tmp')}`] ]);
  assert.equal(readFileSync(f.current, 'utf8'), 'b.png\n');
  assert.equal(readFileSync(f.live, 'utf8'), 'png:b.png');
  assert.equal(statSync(f.current).mode & 0o777, 0o600);
  assert.equal(existsSync(join(f.state, 'active/.wallpaper-live.png.tmp')), false);
});
test('next/previous use current order and wrap without rebuilding the existing package', t => {
  const f = fixture(t); f.prepare(); writeFileSync(f.current, 'c.png\n');
  assert.equal(f.run(), 0); assert.equal(readFileSync(f.current, 'utf8'), 'a.png\n');
  assert.equal(f.run('previous'), 0); assert.equal(readFileSync(f.current, 'utf8'), 'c.png\n');
  assert.ok(f.calls().every(call => call[0] === 'magick'));
});
test('missing or stale current uses preferred; absent preferred starts from first entry', t => {
  for (const [preferred, current, direction, chosen] of [['b.png', '', 'next', 'c.png'], ['b.png', 'deleted.png', 'previous', 'a.png'], ['absent', '', 'next', 'b.png']]) {
    const f = fixture(t, undefined, preferred); f.prepare(); writeFileSync(f.current, current);
    assert.equal(f.run(direction), 0); assert.equal(readFileSync(f.current, 'utf8'), `${chosen}\n`);
  }
});
test('duplicate entries retain the first current/preferred index and original ordering', t => {
  const f = fixture(t, ['a.png', 'b.png', 'a.png', 'c.png']); f.prepare();
  assert.equal(f.run(), 0); assert.equal(readFileSync(f.current, 'utf8'), 'b.png\n');
});
test('empty, single or path-traversal-only lists do not rotate or create live files', t => {
  for (const names of [[], ['a.png'], ['../outside.png', '.', '..', 1, null, true, 'a.png']]) {
    const f = fixture(t, names); f.prepare(); assert.equal(f.run(), 3); assert.deepEqual(f.calls(), []); assert.equal(existsSync(f.live), false);
  }
});
test('unsafe entries are filtered and valid Unicode basenames rotate', t => {
  const f = fixture(t, ['../a', {}, null, 'a.png', '雪.png', 'b.png']); f.prepare();
  assert.equal(f.run(), 0); assert.equal(readFileSync(f.current, 'utf8'), '雪.png\n');
});
test('empty/default selection, Unicode whitespace and invalid mode select the configured default dark theme', t => {
  for (const selection of ['', 'default\n', '\u2007default\u001c\u0085']) {
    const f = fixture(t); f.selection(selection); writeFileSync(join(f.state, 'mode'), 'invalid');
    assert.equal(f.run(), 0); assert.equal(f.calls()[0][4], '/flake with spaces;no-shell#theme-rose');
  }
});
test('explicit palette and light mode resolve independently', t => {
  const f = fixture(t); f.selection('default-palette\n'); writeFileSync(join(f.state, 'mode'), 'light\n');
  assert.equal(f.run(), 0); assert.equal(f.calls()[0][4], '/flake with spaces;no-shell#theme-light-rose');
});
test('overrides, missing palettes and unsafe theme references discard the rotation root and live/current files', t => {
  for (const source of ['/hand-set.png', null, 'nix-theme:', 'nix-theme:a/b', 'nix-theme:a..b', 'nix-theme:a b']) {
    const f = fixture(t); f.prepare(); f.oldLive(); writeFileSync(f.current, 'b.png'); f.source(source);
    assert.equal(f.run(), 3); assert.deepEqual(f.calls(), []);
    for (const path of [f.link, f.current, f.live]) assert.equal(existsSync(path), false);
  }
  const f = fixture(t); f.prepare(); f.selection('not-a-palette'); assert.equal(f.run(), 3); assert.equal(existsSync(f.link), false);
});
test('missing/malformed catalogue leaves existing state intact; invalid object types fail', t => {
  for (const contents of ['bad json', 'null']) {
    const f = fixture(t); f.prepare(); f.oldLive(); writeFileSync(f.catalogue, contents);
    assert.equal(f.run(), 3); assert.equal(readFileSync(f.live, 'utf8'), 'old image'); assert.equal(readlinkSync(f.link), f.theme);
  }
  const f = fixture(t); f.prepare(); rmSync(f.catalogue); assert.equal(f.run(), 3); assert.equal(readlinkSync(f.link), f.theme);
  writeFileSync(f.catalogue, '[]'); assert.equal(f.run(), 1);
});
test('malformed nested catalogue objects fail without deleting rotation state', t => {
  const catalogues = [
    { default: 'x', palettes: ['invalid'] },
    { default: 'x', palettes: { x: 1 } },
    { default: 'x', palettes: { x: { session: 'invalid' } } },
    { default: 'x', palettes: { x: { session: { dark: ['invalid'] } } } },
  ];
  for (const catalogue of catalogues) {
    const f = fixture(t); f.prepare(); f.oldLive(); writeFileSync(f.catalogue, JSON.stringify(catalogue));
    assert.equal(f.run(), 1); assert.equal(readFileSync(f.live, 'utf8'), 'old image'); assert.equal(readlinkSync(f.link), f.theme); assert.deepEqual(f.calls(), []);
  }
});
test('empty nested catalogue values behave as missing theme references', t => {
  for (const palettes of [null, false, 0, '', []]) {
    const f = fixture(t); f.prepare(); f.oldLive(); writeFileSync(f.catalogue, JSON.stringify({default: 'x', palettes}));
    assert.equal(f.run(), 3); assert.equal(existsSync(f.live), false); assert.equal(existsSync(f.link), false);
  }
});
test('missing or malformed theme metadata never converts an image', t => {
  for (const contents of [null, '{broken']) {
    const f = fixture(t); f.prepare(); const path = join(f.theme, 'theme.json');
    if (contents === null) rmSync(path); else writeFileSync(path, contents);
    assert.equal(f.run(), 3); assert.deepEqual(f.calls(), []);
  }
});
test('missing files and symlinks escaping the background directory do not rotate', t => {
  for (const escape of [false, true]) {
    const f = fixture(t); f.prepare(); rmSync(join(f.theme, 'backgrounds/b.png'));
    if (escape) { writeFileSync(join(f.root, 'outside.png'), 'outside'); symlinkSync(join(f.root, 'outside.png'), join(f.theme, 'backgrounds/b.png')); }
    assert.equal(f.run(), 3); assert.deepEqual(f.calls(), []);
  }
});
test('a symlink remaining inside the canonical background directory is accepted', t => {
  const f = fixture(t); f.prepare(); rmSync(join(f.theme, 'backgrounds/b.png')); symlinkSync('c.png', join(f.theme, 'backgrounds/b.png'));
  assert.equal(f.run(), 0); assert.equal(f.calls()[0][1], join(f.theme, 'backgrounds/c.png')); assert.equal(readFileSync(f.current, 'utf8'), 'b.png\n');
});
test('broken package links are rebuilt; a directory package works without a link', t => {
  const f = fixture(t); symlinkSync(join(f.root, 'missing'), f.link); assert.equal(f.run(), 0); assert.equal(f.calls()[0][0], 'nix');
  const other = fixture(t); rmSync(other.theme, { recursive: true }); mkdirSync(join(other.link, 'backgrounds'), { recursive: true }); writeFileSync(join(other.link, 'backgrounds/a.png'), 'a'); writeFileSync(join(other.link, 'backgrounds/b.png'), 'b'); writeFileSync(join(other.link, 'theme.json'), JSON.stringify({ backgrounds: ['a.png', 'b.png'], preferred: 'a.png' }));
  assert.equal(other.run(), 0); assert.equal(other.calls()[0][0], 'magick');
});
test('a concurrent root or current-file change abandons the converted temporary image', t => {
  for (const race of ['link', 'current']) {
    const f = fixture(t); f.prepare(); f.oldLive(); f.db.race = race;
    assert.equal(f.run(), 3); assert.equal(readFileSync(f.live, 'utf8'), 'old image'); assert.equal(existsSync(join(f.state, 'active/.wallpaper-live.png.tmp')), false);
    if (race === 'current') assert.equal(readFileSync(f.current, 'utf8'), 'other.png\n');
  }
});
test('build/conversion failure propagates without replacing existing live/current state', t => {
  for (const fail of ['nix', 'magick']) {
    const f = fixture(t); f.oldLive(); writeFileSync(f.current, 'a.png\n'); f.db.fail = fail;
    assert.equal(f.run(), 1); assert.equal(readFileSync(f.live, 'utf8'), 'old image'); assert.equal(readFileSync(f.current, 'utf8'), 'a.png\n');
  }
});
test('required arguments and invalid directions retain usage exit status', t => {
  const f = fixture(t); assert.equal(f.run('other'), 2);
  const result = spawnSync(legacy ? 'python3' : binary!, legacy ? [legacy] : [], { encoding: 'utf8' }); assert.equal(result.status, 2);
});
