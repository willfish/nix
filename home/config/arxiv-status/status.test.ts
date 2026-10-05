import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';

const bin = process.env.ARXIV_STATUS_BIN;
if (!bin) throw new Error('Set ARXIV_STATUS_BIN to the candidate executable');
function run(t, files = {}, options = {}) {
  const root = mkdtempSync(join(tmpdir(), 'arxiv-status-')); t.after(() => rmSync(root, { recursive: true, force: true }));
  const state = join(root, 'state'), config = join(root, 'config');
  for (const base of [state, config, root, join(root, '.local/state'), join(root, '.config')]) mkdirSync(join(base, 'omarchy-arxiv-scanner'), { recursive: true });
  const bases = { state: state, viewed: state, config: config };
  const names = { state: 'state.json', viewed: 'last_viewed.json', config: 'config.json' };
  for (const [key, value] of Object.entries(files)) {
    const base = options.location === 'empty' ? root : options.location === 'default' ? join(root, key === 'config' ? '.config' : '.local/state') : bases[key];
    writeFileSync(join(base, 'omarchy-arxiv-scanner', names[key]), typeof value === 'string' ? value : JSON.stringify(value));
  }
  const env = { ...process.env, HOME: root }; delete env.XDG_STATE_HOME; delete env.XDG_CONFIG_HOME;
  if (options.location !== 'default') { env.XDG_STATE_HOME = options.location === 'empty' ? '' : state; env.XDG_CONFIG_HOME = options.location === 'empty' ? '' : config; }
  if (options.noHome) delete env.HOME;
  const stdout = execFileSync(bin, options.args || [], { env, cwd: root, encoding: 'utf8' });
  assert.ok(stdout.endsWith('\n')); assert.equal(stdout.trim().split('\n').length, 1);
  return JSON.parse(stdout);
}
const hint = ' · set interests in the panel';
const scan = ' · right-click scans now';
test('missing, malformed and non-object files retain the idle flask and hints', t => {
  for (const state of [undefined, '{broken', '[]', 'null', '42', 'true']) {
    assert.deepEqual(run(t, state === undefined ? {} : { state, config: '[1]', viewed: '"text"' }), { text: '\uf0c3', tooltip: 'arXiv: 0 match(es)' + hint + scan, class: 'idle' });
  }
});
test('matching view timestamps remove the unseen marker and total both arrays', t => {
  assert.deepEqual(run(t, { state: { area_matches: [1, 2], watched_matches: [null], updated_at: 'now' }, viewed: { viewed_at: 'now' }, config: { category: 'cs.AI', interestAreas: ['agents'] } }), { text: '3', tooltip: 'cs.AI: 3 match(es)' + scan, class: 'idle' });
});
test('a changed timestamp marks unseen even with no matches', t => {
  for (const count of [0, 2]) assert.deepEqual(run(t, { state: { area_matches: Array(count).fill(0), updated_at: 'new' }, viewed: { viewed_at: 'old' }, config: { watchedAuthors: [null] } }), { text: count ? '!2' : '\uf0c3', tooltip: `arXiv: ${count} match(es) · not opened since the last scan` + scan, class: 'unseen' });
});
test('only arrays and strings count; empty categories fall back and non-string timestamps are idle', t => {
  for (const category of ['', null, true, 12, {}, []]) assert.deepEqual(run(t, { state: { area_matches: { a: 1 }, watched_matches: 'papers', updated_at: 100 }, config: { category, interestAreas: 'not an array', watchedAuthors: {} } }), { text: '\uf0c3', tooltip: 'arXiv: 0 match(es)' + hint + scan, class: 'idle' });
});
test('Unicode, quotes, line breaks and embedded NULs remain JSON-safe and distinct', t => {
  const category = '数学 "AI"\n\u0000';
  assert.deepEqual(run(t, { state: { updated_at: 'a\u0000b' }, viewed: { viewed_at: 'a\u0000c' }, config: { category } }), { text: '\uf0c3', tooltip: category + ': 0 match(es) · not opened since the last scan' + hint + scan, class: 'unseen' });
});
test('HOME defaults, unset HOME and explicitly empty XDG variables preserve relative paths', t => {
  for (const options of [{ location: 'default' }, { location: 'default', noHome: true }, { location: 'empty' }]) assert.equal(run(t, { state: { area_matches: [1] }, config: { interestAreas: [1] } }, options).text, '1');
});
test('duplicate keys keep the last value and NaN/Infinity in arrays do not lose records', t => {
  assert.deepEqual(run(t, { state: '{"area_matches":[],"area_matches":[NaN,Infinity,-Infinity],"updated_at":"old","updated_at":""}', config: '{"category":"old","category":"new"}' }, { args: ['--ignored'] }), { text: '3', tooltip: 'new: 3 match(es)' + hint + scan, class: 'idle' });
});
