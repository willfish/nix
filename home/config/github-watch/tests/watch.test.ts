import assert from 'node:assert/strict';
import { execFileSync, spawn } from 'node:child_process';
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, symlinkSync, existsSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';

const fixture = process.env.GITHUB_WATCH_FIXTURE, binary = process.env.GITHUB_WATCH_BIN;
if (!fixture || !binary) throw new Error('Set the candidate GITHUB_WATCH_FIXTURE and GITHUB_WATCH_BIN');
function call(operation, data, env = process.env) {
  return JSON.parse(execFileSync(fixture, [operation], { input: JSON.stringify(data), encoding: 'utf8', env, timeout: 5000 }));
}
const item = (id = '1', reason = 'review_requested', url = 'https://api.github.com/repos/acme/widgets/pulls/4') => ({ id, reason, repository: { full_name: 'acme/widgets' }, subject: { title: 'Fix the parser', url } });
const plan = (items, state = { seeded: true, seen: [] }) => call('plan', { items, state });
function sandbox(t, db = {}) {
  const root = mkdtempSync(join(tmpdir(), 'github-watch-')); t.after(() => rmSync(root, { recursive: true, force: true }));
  const commands = join(root, 'bin'); mkdirSync(commands);
  const log = join(root, 'calls.jsonl'), database = join(root, 'db.json'), state = join(root, '.local/state/github-notifications/seen.json');
  writeFileSync(database, JSON.stringify(db));
  const script = `#!${process.execPath}
const fs = require('node:fs'); const path = require('node:path');
const kind = path.basename(process.argv[1]), args = process.argv.slice(2);
const db = JSON.parse(fs.readFileSync(process.env.FAKE_DB, 'utf8'));
fs.appendFileSync(process.env.FAKE_LOG, JSON.stringify({kind,args,pid:process.pid})+'\\n');
if (kind === 'gh') { if (db.ghError) process.stderr.write('fake-private-diagnostic'); process.stdout.write(db.raw ?? JSON.stringify(db.items ?? [])); process.exit(db.ghExit ?? 0); }
if (kind === 'notify-send') {
  const finish = () => { process.stdout.write(db.action ?? ''); process.stderr.write('fake-private-diagnostic'); process.exit(db.notifyExit ?? 0); };
  if (db.waitForState) {
    let tries = 0; const timer = setInterval(() => { if (fs.existsSync(process.env.FAKE_STATE)) { clearInterval(timer); fs.appendFileSync(process.env.FAKE_LOG, JSON.stringify({kind:'state-before-action',seen:JSON.parse(fs.readFileSync(process.env.FAKE_STATE,'utf8')).seen})+'\\n'); finish(); } else if (++tries > 50) { clearInterval(timer); process.exit(9); } }, 10);
  } else finish();
}
if (kind === 'xdg-open') process.exit(0);
`;
  const fake = join(commands, 'fake'); writeFileSync(fake, script, { mode: 0o700 });
  for (const name of ['gh', 'notify-send', 'xdg-open']) symlinkSync(fake, join(commands, name));
  const env = { ...process.env, HOME: root, XDG_STATE_HOME: join(root, 'ignored'), PATH: commands + ':' + process.env.PATH, FAKE_DB: database, FAKE_LOG: log, FAKE_STATE: state };
  return { root, env, state, log, database, calls: () => existsSync(log) ? readFileSync(log, 'utf8').trim().split('\n').map(JSON.parse) : [], seed: (value) => { mkdirSync(join(root, '.local/state/github-notifications'), { recursive: true }); writeFileSync(state, typeof value === 'string' ? value : JSON.stringify(value)); } };
}
function run(env) {
  return new Promise(resolve => {
    const child = spawn(binary, [], { env }); let stdout = '', stderr = '';
    child.stdout.on('data', x => stdout += x); child.stderr.on('data', x => stderr += x);
    const timer = setTimeout(() => child.kill('SIGKILL'), 5000);
    child.on('close', (code, signal) => { clearTimeout(timer); resolve({ code, signal, stdout, stderr }); });
  });
}

test('API issues, pull requests, commits, discussions and comment fallback become GitHub pages', () => {
  for (const [input, expected] of [
    ['repos/acme/widgets/issues/12', 'acme/widgets/issues/12'], ['repos/acme/widgets/pulls/4', 'acme/widgets/pull/4'],
    ['repos/acme/widgets/issues/12/comments/9', 'acme/widgets/issues/12'], ['repos/acme/widgets/commits/abc', 'acme/widgets/commit/abc'],
    ['repos/acme/widgets/discussions/7', 'acme/widgets/discussions/7'], ['/repos//acme/widgets/pulls/4?token=discard#fragment', 'acme/widgets/pull/4']
  ]) assert.equal(call('url', 'https://api.github.com/' + input), 'https://github.com/' + expected);
  assert.equal(call('url', 'HTTPS://github.com/acme/widgets/issues/1?query=discard#fragment'), 'https://github.com/acme/widgets/issues/1');
  assert.equal(call('url', ' \nhttps://api.github.com/repos/acme/widgets/pulls/4\t'), 'https://github.com/acme/widgets/pull/4');
});
test('URL trust rejects credentials, explicit ports, wrong hosts/schemes, host roots and numberless comment APIs', () => {
  for (const url of ['', 'http://github.com/r', 'https://github.com', 'https://github.com/', 'https://github.com:443/r', 'https://u@github.com/r', 'https://GitHub.com/r', 'https://github.com.evil/r', 'https://api.github.com/repos/a/b/issues/comments/1', 'https://api.github.com/repos/a/b/issues', 'https://api.github.com/repos/a/b/pulls', 'https://api.github.com/repos/a/b/releases/1']) assert.equal(call('url', url), '', url);
});
test('all reason labels and unknown reasons remain stable', () => {
  const labels = { assign: 'Assigned', author: 'Update', comment: 'Comment', ci_activity: 'CI', invitation: 'Invitation', manual: 'Subscribed', mention: 'Mention', review_requested: 'Review requested', security_alert: 'Security alert', state_change: 'State change', subscribed: 'Subscribed', team_mention: 'Team mention', other: 'Notification' };
  for (const [reason, label] of Object.entries(labels)) assert.equal(plan([item('1', reason)]).announcements[0].body, label + ': Fix the parser\nhttps://github.com/acme/widgets/pull/4');
});
test('first poll gives only the backlog summary, seeds empty polls, and replaces an unseeded old seen set', () => {
  assert.deepEqual(plan([item('2'), item('1'), item('1'), { id: '' }], { seen: ['stale'], seeded: false }), { announcements: [{ summary: 'GitHub', body: '3 unread notifications already waiting' }], seen: ['1', '2'], seeded: true });
  assert.deepEqual(plan([], { seeded: false, seen: [] }), { announcements: [], seen: [], seeded: true });
});
test('deduplication retains disappearing IDs and preserves response order rather than sorting alerts', () => {
  const result = plan([item('z'), item('old'), item('a')], { seeded: true, seen: ['old', 'gone'] });
  assert.equal(result.announcements.length, 2); assert.deepEqual(result.seen, ['a', 'gone', 'old', 'z']);
  assert.deepEqual(plan([item('a'), item('z')], result).announcements, []);
});
test('eight alerts plus the overflow summary mark every fresh and duplicate thread seen', () => {
  const result = plan(Array.from({ length: 11 }, (_, i) => item(String(i))));
  assert.equal(result.announcements.length, 9); assert.equal(result.announcements[8].body, '3 more new notifications'); assert.equal(result.seen.length, 11);
  const duplicate = plan(Array.from({ length: 9 }, () => item('same')));
  assert.equal(duplicate.announcements.length, 9); assert.deepEqual(duplicate.seen, ['same']);
});
test('fallback subjects, missing IDs, numeric IDs and latest-comment URLs preserve alert fields', () => {
  const result = plan([{ id: 12, subject: { latest_comment_url: 'https://api.github.com/repos/acme/widgets/issues/12/comments/9' } }, { subject: { title: 'ignored' } }]);
  assert.deepEqual(result.seen, ['12']); assert.deepEqual(result.announcements, [{ summary: 'GitHub', body: 'Notification: GitHub notification\nhttps://github.com/acme/widgets/issues/12', url: 'https://github.com/acme/widgets/issues/12' }]);
});
test('seen identity strings include Unicode and embedded NULs without truncating or merging', () => {
  const result = plan([item('a\u0000c'), item('数学')], { seeded: true, seen: ['a\u0000b'] });
  assert.equal(result.announcements.length, 2); assert.deepEqual(result.seen, ['a\u0000b', 'a\u0000c', '数学']);
});
test('notify-send argv retains the desktop entry, icon, priority, expiry, separator and Open action', () => {
  const notice = plan([item()]).announcements[0];
  assert.deepEqual(call('command', notice), ['notify-send', '-a', 'GitHub', '-i', 'github', '-u', 'normal', '-t', '30000', '-h', 'string:desktop-entry:github-notifications', '-A', 'open=Open', '--', 'acme/widgets', 'Review requested: Fix the parser\nhttps://github.com/acme/widgets/pull/4']);
  assert.deepEqual(call('command', { summary: '--unsafe', body: '$(not a shell)', url: '' }).slice(-3), ['--', '--unsafe', '$(not a shell)']);
});
test('only the trimmed Open action launches a browser, including Unicode whitespace', t => {
  const s = sandbox(t); const url = 'https://github.com/a/b/issues/1';
  for (const output of ['open\n', '\u2003open\u00a0']) assert.equal(call('open', { output, url }, s.env), true);
  for (const output of ['', 'closed', 'Open', 'open something']) assert.equal(call('open', { output, url }, s.env), false);
  assert.equal(call('open', { output: 'open', url: '' }, s.env), false);
  assert.deepEqual(s.calls().map(x => x.args), [[url], [url]]);
});
test('subprocess capture suppresses diagnostics, reports failure, handles absent commands and kills timeouts', () => {
  assert.deepEqual(call('capture', { argv: ['sh', '-c', 'printf data; printf secret >&2; exit 4'], timeout_ms: 1000 }), { started: true, ok: false, timed_out: false, output: 'data' });
  assert.equal(call('capture', { argv: ['no-such-github-watch-fixture-command'], timeout_ms: 1000 }).started, false);
  const started = Date.now(); const result = call('capture', { argv: ['sh', '-c', 'exec sleep 10'], timeout_ms: 100 });
  assert.equal(result.timed_out, true); assert.equal(result.ok, false); assert.ok(Date.now() - started < 2000);
});
test('CLI first poll uses only the read-only gh command, saves sorted state and exits without opening', async t => {
  const s = sandbox(t, { items: [item('2'), item('1')], action: 'open' });
  assert.deepEqual(await run(s.env), { code: 0, signal: null, stdout: '', stderr: '' });
  assert.deepEqual(s.calls().map(x => x.kind), ['gh', 'notify-send']); assert.deepEqual(s.calls()[0].args, ['api', '--paginate', 'notifications']);
  assert.equal(s.calls()[1].args.at(-1), '2 unread notifications already waiting');
  assert.deepEqual(JSON.parse(readFileSync(s.state, 'utf8')), { seeded: true, seen: ['1', '2'] }); assert.ok(!existsSync(s.state.replace('.json', '.tmp')));
});
test('CLI joins notification actions after saving state, ignores notify exit status, and never invokes a shell', async t => {
  const s = sandbox(t, { items: [item('1'), item('2')], action: 'open\n', waitForState: true, notifyExit: 4 }); s.seed({ seeded: true, seen: ['gone'] });
  assert.deepEqual(await run(s.env), { code: 0, signal: null, stdout: '', stderr: '' });
  const calls = s.calls(); assert.equal(calls.filter(x => x.kind === 'notify-send').length, 2); assert.equal(calls.filter(x => x.kind === 'xdg-open').length, 2);
  for (const seen of calls.filter(x => x.kind === 'state-before-action')) assert.deepEqual(seen.seen, ['1', '2', 'gone']);
  for (const open of calls.filter(x => x.kind === 'xdg-open')) assert.deepEqual(open.args, ['https://github.com/acme/widgets/pull/4']);
  assert.ok(!existsSync(join(s.root, 'ignored/github-notifications/seen.json')));
});
test('CLI repeats exit promptly without notifications or browser processes', async t => {
  const s = sandbox(t, { items: [item()], action: 'closed' }); s.seed({ seeded: true, seen: ['1'] });
  assert.equal((await run(s.env)).code, 0); assert.deepEqual(s.calls().map(x => x.kind), ['gh']);
});
test('CLI gh errors, empty output, malformed JSON and concatenated pages never change state or show alerts', async t => {
  for (const db of [{ ghExit: 1, ghError: true }, { raw: '' }, { raw: '{bad' }, { raw: '[]\n[]' }]) {
    const s = sandbox(t, db); const old = '{"seeded":true, "seen":["old"]}\n'; s.seed(old);
    assert.deepEqual(await run(s.env), { code: 0, signal: null, stdout: '', stderr: '' });
    assert.equal(readFileSync(s.state, 'utf8'), old); assert.deepEqual(s.calls().map(x => x.kind), ['gh']);
  }
});
test('CLI supports object items and malformed saved JSON reseeds instead of replaying a burst', async t => {
  const s = sandbox(t, { raw: JSON.stringify({ items: [item('5')] }) }); s.seed('{broken');
  assert.equal((await run(s.env)).code, 0); assert.equal(s.calls().find(x => x.kind === 'notify-send').args.at(-1), '1 unread notifications already waiting');
});
test('CLI closed actions and untrusted URLs do not launch browsers', async t => {
  for (const db of [{ items: [item()], action: 'closed' }, { items: [item('1', 'mention', 'https://evil.example/private')], action: 'open' }]) {
    const s = sandbox(t, db); s.seed({ seeded: true, seen: [] }); assert.equal((await run(s.env)).code, 0);
    assert.equal(s.calls().filter(x => x.kind === 'xdg-open').length, 0);
  }
});
