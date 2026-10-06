import assert from 'node:assert/strict';
import { execFile, execFileSync, spawnSync } from 'node:child_process';
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, readdirSync, rmSync, statSync, symlinkSync, chmodSync } from 'node:fs';
import { createServer } from 'node:http';
import { createServer as createHttpsServer } from 'node:https';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';

const fixture = process.env.AGENT_USAGE_FIXTURE!;
const bins = process.env.AGENT_USAGE_BIN_DIR!;
const now = Date.parse('2026-09-29T12:00:00Z') / 1000;
function environment(root: string, extra = {}) {
  return { ...process.env, HOME: root, PI_AGENT_DIR: join(root, 'agent'), OPENCODE_GO_API_KEY: '', XDG_STATE_HOME: join(root, 'state'), TZ: 'UTC', ...extra };
}
function dir(t) { const root = mkdtempSync(join(tmpdir(), 'usage-test-')); t.after(() => rmSync(root, { recursive: true, force: true })); return root; }
function save(path: string, value: any) { mkdirSync(join(path, '..'), { recursive: true }); writeFileSync(path, JSON.stringify(value)); }
function call(op: string, args: any = {}, env = process.env) {
  return JSON.parse(execFileSync(fixture, [op], { input: JSON.stringify({ now, ...args }) + '\n', encoding: 'utf8', maxBuffer: 32 * 1024 * 1024, env: { ...env, TZ: 'UTC' } }));
}
function billing(value) { return call('billing', { value }); }
const usage = { input: 10, output: 2, reasoning: 3, cacheRead: 1, cacheWrite: 0 };
function event(provider = 'openai-codex', model = 'gpt-6-astra', timestamp = now * 1000) {
  return { timestamp, message: { role: 'assistant', provider, model, timestamp, usage, content: 'PRIVATE MESSAGE MUST NOT APPEAR' } };
}
function auth(root: string, values: any) { save(join(root, 'agent/auth.json'), values); }
function collect(id: string, root: string, replies: any[] = []) { return call('collect', { id, replies }, environment(root)); }
const codexBody = { plan_type: 'pro', rate_limit: { primary_window: { used_percent: 1, limit_window_seconds: 604800, reset_after_seconds: 3600 } } };

test('numbers, fractions and provider percentages retain their distinct scales', () => {
  for (const [value, expected] of [[null, 0], ['bad', 0], ['2.5', 2], [3.5, 4], [true, 1]]) assert.equal(call('number', { value }), expected);
  for (const [value, expected] of [[1, 1], [4, .04], ['40%', .4], ['bad', -1], [-1, -1], [101, 1], [null, -1]]) assert.equal(call('fraction', { value }), expected);
  assert.equal(call('fraction', { value: 1, percent: true }), .01);
});

test('configured keys, provider labels and numeric/date text retain whitespace handling', () => {
  assert.equal(call('command', { value: '\u00a0fixture-key\u00a0', timeout_ms: 1000 }), 'fixture-key');
  assert.equal(call('command', { value: "!printf '\\302\\240fixture-key\\302\\240'", timeout_ms: 1000 }), 'fixture-key');
  assert.equal(call('codex', { value: { plan_type: '\u00a0pro_plus\u00a0' } }).plan, 'Pro Plus');
  assert.equal(call('tier', { value: { plan: '\u00a0go_plus\u00a0' } }), 'go plus');
  assert.equal(call('tier', { value: { tier: ' 5 ' } }), 'SuperGrok Heavy');
  assert.equal(call('number', { value: '\u00a02.5\u00a0' }), 2);
  assert.equal(call('fraction', { value: '40%\u00a0' }), .4);
  assert.equal(call('timestamp', { value: '\u00a01791050635\u00a0' }), '2026-10-03T18:03:55+00:00');
  assert.equal(call('day', { value: '\u00a01791050635\u00a0' }), '2026-10-03');
  assert.equal(call('day', { value: '\u00a02026-10-03T12:00:00Z\u00a0' }), '2026-10-03');
  assert.equal(call('number', { value: '\u001c2\u001c' }), 0);
  assert.equal(call('tier', { value: { tier: '\u001c5\u001c' } }), '');
});

test('timestamps, milliseconds, offsets, malformed dates and recent calendar days', () => {
  assert.equal(call('day', { value: now * 1000 }), '2026-09-29');
  assert.equal(call('day', { value: String(now) }), '2026-09-29');
  assert.equal(call('day', { value: '2026-09-30T00:30:00+02:00' }), '2026-09-29');
  for (const value of [null, '', 'bad', '2026-02-30']) assert.equal(call('day', { value }), '2026-09-29');
  assert.equal(call('timestamp', { value: 1791050635 }), '2026-10-03T18:03:55+00:00');
  assert.equal(call('timestamp', { value: now * 1000 }), '2026-09-29T12:00:00+00:00');
  assert.equal(call('timestamp', { value: 'unchanged' }), 'unchanged');
  assert.deepEqual(call('empty').recentDays.map(x => x.date), ['2026-09-23', '2026-09-24', '2026-09-25', '2026-09-26', '2026-09-27', '2026-09-28', '2026-09-29']);
});

test('Pi scans only matching assistant usage, includes reasoning and never message text', t => {
  const root = dir(t); const path = join(root, 'nested/session.jsonl'); mkdirSync(join(path, '..'), { recursive: true });
  const records = [event(), event('xai', 'grok-4.7'), { message: { ...event().message, role: 'user' } }, [], {}, { message: { provider: 'openai-codex', model: 'zero' } }];
  writeFileSync(path, records.map(x => JSON.stringify(x)).join('\n') + '\nmalformed\n');
  const stats = call('pi', { path: root, provider: 'openai-codex' });
  assert.equal(stats.totalPrompts, 2); assert.equal(stats.totalSessions, 1); assert.equal(stats.todayTotalTokens, 16);
  assert.equal(stats.modelUsage['gpt-6-astra'].outputTokens, 5); assert.equal(stats.recentDays[6].messageCount, 16);
  assert.equal(stats.todaySessions, 1); assert.deepEqual(stats.activeDates, ['2026-09-29']);
  assert.ok(!JSON.stringify(stats).includes('PRIVATE MESSAGE'));
  assert.equal(call('pi', { path: root, provider: 'xai' }).totalPrompts, 1);
  assert.equal(call('pi', { path: join(root, 'missing'), provider: 'xai' }).totalSessions, 0);
  symlinkSync(root, join(root, 'loop')); assert.equal(call('pi', { path: root, provider: 'xai' }).totalPrompts, 1);
  writeFileSync(path, Buffer.concat([Buffer.from('{"message":{"provider":"openai-codex","model":"gpt-'), Buffer.from([255]), Buffer.from('","usage":{"input":1}}}\n')]));
  assert.equal(call('pi', { path: root, provider: 'openai-codex' }).modelUsage['gpt-�'].inputTokens, 1);
});

test('Grok keeps the largest session snapshot, subtracts cached input and counts turns once across models', t => {
  const root = dir(t), path = join(root, 'demo/session-a/updates.jsonl'); mkdirSync(join(path, '..'), { recursive: true });
  const snapshot = (totalTokens, numTurns, inputTokens, outputTokens, cachedReadTokens) => ({ timestamp: now, params: { sessionId: 'session-a', update: { sessionUpdate: 'turn_completed', usage: { totalTokens, numTurns, inputTokens, outputTokens, cachedReadTokens, modelUsage: { 'grok-4.5': { inputTokens, outputTokens, cachedReadTokens } } } } } });
  writeFileSync(path, [snapshot(100, 1, 80, 20, 10), snapshot(250, 2, 200, 50, 40), snapshot(110, 1, 90, 20, 5), { params: { update: { sessionUpdate: 'tool_call', usage: { totalTokens: 9999 } } } }].map(x => JSON.stringify(x)).join('\n'));
  const stats = call('grok', { path: root }); assert.equal(stats.totalSessions, 1); assert.equal(stats.totalPrompts, 2); assert.equal(stats.modelUsage['grok-4.5'].inputTokens, 160);
  writeFileSync(path, JSON.stringify({ timestamp: now, params: { update: { sessionUpdate: 'turn_completed', usage: { inputTokens: 5, cachedReadTokens: 9, numTurns: 3 } } } }));
  const fallback = call('grok', { path: root }); assert.equal(fallback.totalPrompts, 3); assert.equal(fallback.modelUsage.grok.inputTokens, 5);
});

test('OpenCode storage scans include only Go models/providers and all token buckets', t => {
  const root = dir(t);
  save(join(root, 'message/ses/go.json'), { role: 'assistant', providerID: 'opencode', modelID: 'deepseek-v4-flash', sessionID: 'ses', time: { created: now * 1000 }, tokens: { input: 10, output: 4, reasoning: 1, cache: { read: 2, write: 3 } } });
  save(join(root, 'message/ses/router.json'), { role: 'assistant', providerID: 'openrouter', modelID: 'other', tokens: { input: 100 } });
  const stats = call('opencode', { path: root }); assert.equal(stats.totalPrompts, 1); assert.equal(stats.modelUsage['deepseek-v4-flash'].outputTokens, 5); assert.equal(stats.todayTotalTokens, 20);
});

test('Grok billing reads weekly/monthly pool, separate Build share and proto3 omitted zero', () => {
  const end = '2026-10-08T20:25:49+00:00';
  for (const percent of [0, 1, 1.0, 2, 32, 100, 150]) {
    const limits = billing({ config: { creditUsagePercent: percent, currentPeriod: { type: 'USAGE_PERIOD_TYPE_WEEKLY', end }, productUsage: [{ product: 'GrokBuild', usagePercent: percent }, { product: 'GrokChat' }] } });
    assert.equal(limits[0].percent, Math.min(1, percent / 100)); assert.equal(limits[1].percent, Math.min(1, percent / 100)); assert.equal(limits[0].title, 'Weekly'); assert.equal(limits[1].title, 'Grok Build');
  }
  const omitted = billing({ config: { currentPeriod: { end }, onDemandCap: { val: 0 }, isUnifiedBillingUser: true } });
  assert.equal(omitted.length, 1); assert.equal(omitted[0].percent, 0); assert.equal(omitted[0].resetsAt, end);
  assert.deepEqual(billing({ config: { isUnifiedBillingUser: true } }), []);
  assert.deepEqual(billing({ config: { currentPeriod: { end }, creditUsagePercent: 'nope' } }), []);
  assert.equal(billing({ current_period: { type: 'MONTHLY', end }, credit_usage_percent: 50 })[0].title, 'Monthly');
});

test('OpenCode windows and upgraded plan normalization remain on the Go record', () => {
  const value = { plan: 'go plus', usage: { rolling: { percent: 4, resetsAt: 'reset' }, weekly: { percent: 8 }, monthly: { percent: 2 } } };
  const result = call('go', { value }); assert.equal(result.plan, 'Go Plus'); assert.deepEqual(result.limits.map(x => x.title), ['Session', 'Weekly', 'Monthly']); assert.equal(result.limits[0].percent, .04);
  assert.equal(call('go', { value: { rolling: { usagePercent: .4, resetAt: 'reset' } } }).plan, 'Go');
});

test('Codex windows always use a 0-100 percent scale and readable UTC resets', () => {
  const result = call('codex', { value: codexBody }); assert.equal(result.plan, 'Pro'); assert.equal(result.status, ''); assert.equal(result.limits[0].percent, .01); assert.equal(result.limits[0].resetsAt, '2026-09-29T13:00:00+00:00');
  const full = call('codex', { value: { rate_limit: { allowed: false, primary_window: { used_percent: 100, limit_window_seconds: 18000, reset_at: 1791050635 } } } });
  assert.equal(full.limits[0].percent, 1); assert.equal(full.limits[0].label, '5h window'); assert.equal(full.status, 'Rate limit reached'); assert.match(full.limits[0].resetsAt, /^2026-10-03T/);
});

test('Waybar retains icon, alarm threshold, subscription lines and missing-data messages', () => {
  const records = [{ id: 'codex', name: 'Codex', tierLabel: 'Max 20x', limits: [{ title: 'Session', percent: .4 }] }, { id: 'grok', name: 'Grok', tierLabel: 'SuperGrok', limits: [{ title: 'Weekly', percent: .91 }] }];
  const result = call('waybar', { value: records }); assert.equal(result.text, '󱚣'); assert.equal(result.class, 'alarm'); assert.match(result.tooltip, /Codex · Max 20x · Session 40%/); assert.match(result.tooltip, /Grok · SuperGrok · Weekly 91%/); assert.ok(!result.tooltip.toLowerCase().includes('token'));
  assert.deepEqual(call('waybar', { value: [] }), { text: '󱚣', class: 'idle', tooltip: 'Agents · collecting usage' });
  assert.equal(call('waybar', { value: [{ id: 'c', limits: [], authHelpText: 'Login' }, null, {}] }).tooltip, 'Agents\nc · Login');
});

test('legacy credential helpers retain login order, profile fields, tiers and Go-key preference', () => {
  assert.equal(call('tier', { value: { tier: 5 } }), 'SuperGrok Heavy'); assert.equal(call('tier', { value: { plan: ' go_plus ' } }), 'go plus');
  assert.equal(call('login', { value: { bad: {}, expired: { key: 'old', expires_at: '2000-01-01' }, current: { key: 'fixture' } } }).key, 'current');
  const merged = call('merge_login', { entry: { key: 'old', refresh_token: 'old-refresh', email: 'kept@example' }, payload: { access_token: 'new', refresh_token: 'new-refresh', expires_in: 3600 } });
  assert.equal(merged.key, 'new'); assert.equal(merged.email, 'kept@example'); assert.equal(merged.expires_at, '2026-09-29T13:00:00Z');
  assert.equal(call('key', { value: { 'opencode-go': { key: 'fixture' }, opencode: 'other' } }), 'fixture'); assert.equal(call('key', { value: {} }), '');
});

test('Pi token validity preserves refresh skew and rotated fields without losing account or extensions', () => {
  const entry = { type: 'oauth', access: 'old', refresh: 'old', accountId: 'acct', extension: { keep: true }, expires: now * 1000 + 200000 };
  assert.equal(call('current', { value: entry, now_ms: now * 1000 }), false); assert.equal(call('current', { value: entry, now_ms: now * 1000, skew_ms: 0 }), true);
  const merged = call('merge', { entry, payload: { access_token: 'new', refresh_token: 'newer', expires_in: 3600 }, now_ms: now * 1000 });
  assert.equal(merged.access, 'new'); assert.equal(merged.refresh, 'newer'); assert.equal(merged.accountId, 'acct'); assert.deepEqual(merged.extension, { keep: true }); assert.equal(merged.expires, now * 1000 + 3300000);
});

test('JWT claims accept unpadded URL-safe payloads and reject malformed content', () => {
  const claims = { principal_id: 'user', tier: 5 }; const token = 'header.' + Buffer.from(JSON.stringify(claims)).toString('base64url') + '.signature';
  assert.deepEqual(call('jwt', { value: token }), claims);
  for (const value of ['', 'bad', 'x.!?.y', 'x.W10.y']) assert.deepEqual(call('jwt', { value }), {});
});

test('trusted command keys are trimmed, errors suppressed and process groups bounded', () => {
  assert.equal(call('command', { value: ' plain-key ', timeout_ms: 1000 }), 'plain-key');
  assert.equal(call('command', { value: "!printf ' fixture-key\\n'; printf diagnostic >&2", timeout_ms: 1000 }), 'fixture-key');
  assert.equal(call('command', { value: '!exit 1', timeout_ms: 1000 }), '');
  const start = Date.now(); assert.equal(call('command', { value: '!sleep 10', timeout_ms: 30 }), ''); assert.ok(Date.now() - start < 2000);
});

test('all installed command names work offline, including ignored updater flags and exact help', t => {
  const root = dir(t), env = environment(root);
  for (const id of ['codex', 'grok', 'opencode']) {
    const bin = join(bins, 'omarchy-agent-usage-' + id);
    const output = spawnSync(bin, ['--force', '--limits-only'], { env, encoding: 'utf8' }); assert.equal(output.status, 0); assert.equal(output.stderr, '');
    const record = JSON.parse(output.stdout); assert.equal(record.id, id); assert.equal(record.schemaVersion, 1); assert.equal(record.ready, true); assert.equal(record.hasLocalStats, true); assert.equal(record.usageStatusText, 'Waiting for auth'); assert.ok(!('retryAdvised' in record));
    const help = spawnSync(bin, ['--help'], { env, encoding: 'utf8' }); assert.equal(help.status, 0); assert.equal(help.stdout, ''); assert.equal(help.stderr, `usage: omarchy-agent-usage-${id} [--force] [--limits-only]\n`);
  }
});

test('status executable loads sorted object records only from the existing state path', t => {
  const root = dir(t), path = join(root, 'state/omarchy/agents/usage'); mkdirSync(path, { recursive: true });
  save(join(path, 'b.json'), { id: 'grok', name: 'Grok', limits: [{ title: 'Weekly', percent: .9 }] }); save(join(path, 'a.json'), { id: 'codex', name: 'Codex' }); save(join(path, 'array.json'), []); writeFileSync(join(path, 'bad.json'), '{');
  const result = JSON.parse(execFileSync(join(bins, 'hypr-agent-status'), { env: environment(root), encoding: 'utf8' })); assert.equal(result.class, 'alarm'); assert.equal(result.tooltip, 'Agents\nCodex\nGrok · Weekly 90%');
});

test('fresh OAuth credentials use fixed usage URLs and required account/JWT headers without refresh', t => {
  const root = dir(t); auth(root, { 'openai-codex': { access: 'fixture-access', expires: now * 1000 + 900000, accountId: 'acct' } });
  const output = collect('codex', root, [{ status: 200, body: codexBody }]); assert.equal(output.requests.length, 1); assert.equal(output.requests[0].url, 'https://chatgpt.com/backend-api/wham/usage'); assert.equal(output.requests[0].headers, 'ChatGPT-Account-Id: acct'); assert.equal(output.record.tierLabel, 'Pro'); assert.equal(output.record.limits[0].percent, .01); assert.ok(!JSON.stringify(output.record).includes('fixture-access'));
  const token = 'h.' + Buffer.from(JSON.stringify({ principal_id: 'user', tier: 5 })).toString('base64url') + '.s';
  auth(root, { xai: { access: token, expires: now * 1000 + 900000 } });
  const grok = collect('grok', root, [{ status: 200, body: { currentPeriod: { end: 'reset' } } }]); assert.equal(grok.record.tierLabel, 'SuperGrok Heavy'); assert.match(grok.requests[0].headers, /x-userid: user/); assert.equal(grok.record.limits[0].percent, 0);
});

test('OAuth refresh persists all profiles atomically at mode 0600 and rotates the bearer used for probing', t => {
  const root = dir(t), original = { 'openai-codex': { type: 'oauth', access: 'old', refresh: 'refresh with &', expires: 1, accountId: 'acct', extra: 42 }, other: { keep: 'unchanged' } }; auth(root, original);
  const output = collect('codex', root, [{ status: 200, body: { access_token: 'fixture-new', refresh_token: 'new-refresh', expires_in: 3600 } }, { status: 200, body: codexBody }]);
  assert.equal(output.requests[0].url, 'https://auth.openai.com/oauth/token'); assert.equal(new URLSearchParams(output.requests[0].form).get('refresh_token'), 'refresh with &'); assert.equal(output.requests[1].token, 'fixture-new');
  const path = join(root, 'agent/auth.json'), stored = JSON.parse(readFileSync(path, 'utf8')); assert.equal(stored['openai-codex'].accountId, 'acct'); assert.equal(stored['openai-codex'].extra, 42); assert.deepEqual(stored.other, original.other); assert.equal(statSync(path).mode & 0o777, 0o600); assert.deepEqual(readdirSync(join(root, 'agent')), ['auth.json']);
});

test('failed credential publication reports the failure without probing with unsaved tokens', t => {
  const root = dir(t), folder = join(root, 'agent'), path = join(folder, 'auth.json');
  auth(root, { 'openai-codex': { access: 'fixture-old', refresh: 'fixture-refresh', expires: 1 } });
  const before = readFileSync(path, 'utf8');
  chmodSync(folder, 0o500);
  try {
    const output = collect('codex', root, [{ status: 200, body: { access_token: 'fixture-new', refresh_token: 'fixture-rotated', expires_in: 3600 } }]);
    assert.equal(output.requests.length, 1);
    assert.equal(readFileSync(path, 'utf8'), before);
    assert.equal(output.record.usageStatusText, "Couldn't save refreshed credentials");
    assert.match(output.record.authHelpText, /writable/);
    assert.doesNotMatch(JSON.stringify(output.record), /fixture-(old|new|rotated|refresh)/);
    assert.deepEqual(readdirSync(folder), ['auth.json']);
  } finally { chmodSync(folder, 0o700); }
});

test('allocation size overflow exits with a value-free diagnostic', () => {
  for (const op of ['overflow_add', 'overflow_multiply']) {
    const output = spawnSync(fixture, [op], { input: '{}\n', encoding: 'utf8' });
    assert.equal(output.status, 1);
    assert.equal(output.stdout, '');
    assert.equal(output.stderr, 'agent-usage: allocation failed\n');
  }
});

test('failed refresh falls back only to a still-valid token, and missing refresh stops expired probes', t => {
  const root = dir(t); auth(root, { xai: { access: 'fixture', refresh: 'fixture-refresh', expires: now * 1000 + 10000 } });
  const fallback = collect('grok', root, [{ failed: true }, { status: 200, body: { currentPeriod: { end: 'reset' } } }]); assert.equal(fallback.requests.length, 2); assert.equal(fallback.record.usageStatusText, '');
  auth(root, { xai: { access: 'old', refresh: 'r', expires: 1 } }); const expired = collect('grok', root, [{ failed: true }]); assert.equal(expired.requests.length, 1); assert.equal(expired.record.usageStatusText, 'Sign-in expired');
  auth(root, { xai: { access: 'old', expires: 1 } }); assert.equal(collect('grok', root).requests.length, 0);
});

test('Go key precedence is environment, models trusted command, then auth; diagnostics never enter records', t => {
  const root = dir(t); save(join(root, 'agent/models.json'), { providers: { 'opencode-go': { apiKey: '!printf command-key; printf HIDDEN >&2' } } }); auth(root, { 'opencode-go': { key: 'auth-key' } });
  const replies = [{ status: 200, body: { plan: 'go plus', rolling: { percent: .4 } } }];
  const command = collect('opencode', root, replies); assert.equal(command.requests[0].token, 'command-key'); assert.equal(command.record.tierLabel, 'Go Plus'); assert.ok(!JSON.stringify(command.record).includes('HIDDEN'));
  const env = call('collect', { id: 'opencode', replies }, environment(root, { OPENCODE_GO_API_KEY: 'env-key' })); assert.equal(env.requests[0].token, 'env-key');
  save(join(root, 'agent/models.json'), {}); assert.equal(collect('opencode', root, replies).requests[0].token, 'auth-key');
});

test('HTTP status, network errors and missing allowances retain useful local stats and retry semantics', t => {
  const root = dir(t); auth(root, { 'openai-codex': { access: 'fixture', expires: now * 1000 + 900000 } }); mkdirSync(join(root, 'agent/sessions'), { recursive: true }); writeFileSync(join(root, 'agent/sessions/s.jsonl'), JSON.stringify(event()));
  const status = collect('codex', root, [{ status: 401 }]).record; assert.equal(status.authHelpText, 'Codex usage returned status 401.'); assert.ok(!status.retryAdvised); assert.equal(status.totalPrompts, 1);
  const failed = collect('codex', root, [{ failed: true }]).record; assert.equal(failed.retryAdvised, true); assert.equal(failed.totalPrompts, 1);
  assert.equal(collect('codex', root, [{ status: 200, body: {} }]).record.authHelpText, 'Codex usage returned no allowance.');
  auth(root, { 'opencode-go': 'fixture-key' }); assert.equal(collect('opencode', root, [{ status: 403 }]).record.authHelpText, 'This OpenCode key has no Go subscription.');
});

async function probe(args, env = process.env) {
  return await new Promise<any>((resolve, reject) => {
    const child = execFile(fixture, ['probe'], { env, maxBuffer: 32 * 1024 * 1024 }, (error, stdout) => error ? reject(error) : resolve(JSON.parse(stdout)));
    child.stdin!.end(JSON.stringify(args) + '\n');
  });
}
async function listen(server, t) {
  server.on('tlsClientError', () => {}); await new Promise<void>(resolve => server.listen(0, '127.0.0.1', resolve)); t.after(() => new Promise<void>(resolve => server.close(resolve))); return server.address().port;
}

test('real HTTP transport sends bearer/form data, rejects bad JSON, preserves large responses and never follows redirects', async t => {
  const seen = []; const server = createServer((req, res) => {
    seen.push({ path: req.url, auth: req.headers.authorization, type: req.headers['content-type'] });
    if (req.url === '/redirect') { res.writeHead(302, { Location: '/capture' }); res.end(); }
    else if (req.url === '/error') { res.writeHead(503, { 'Content-Length': '100' }); res.flushHeaders(); }
    else if (req.url === '/invalid') res.end('[]');
    else if (req.url === '/nul') res.end('{"ok":true}\0trailing');
    else if (req.url === '/large') res.end(JSON.stringify({ padding: 'x'.repeat(9 * 1024 * 1024) }));
    else { let data = ''; req.on('data', chunk => data += chunk); req.on('end', () => res.end(JSON.stringify({ received: data }))); }
  });
  const port = await listen(server, t); const url = `http://127.0.0.1:${port}`;
  const post = await probe({ url, token: 'fixture-access', form: 'grant_type=refresh_token' }); assert.equal(post.failed, false); assert.equal(post.body.received, 'grant_type=refresh_token'); assert.equal(seen[0].auth, 'Bearer fixture-access'); assert.equal(seen[0].type, 'application/x-www-form-urlencoded');
  const redirect = await probe({ url: url + '/redirect', token: 'fixture' }); assert.equal(redirect.status, 302); assert.ok(!seen.some(x => x.path === '/capture'));
  const start = Date.now(); const error = await probe({ url: url + '/error' }); assert.equal(error.status, 503); assert.equal(error.failed, false); assert.ok(Date.now() - start < 2000);
  assert.equal((await probe({ url: url + '/invalid' })).failed, true); assert.equal((await probe({ url: url + '/nul' })).failed, true); assert.equal((await probe({ url: url + '/large' })).body.padding.length, 9 * 1024 * 1024);
  const count = seen.length; assert.equal((await probe({ url, token: 'bad\r\nInjected: value' })).failed, true); assert.equal(seen.length, count);
});

test('HTTP responses may exceed fifteen seconds while still making progress', async t => {
  const server = createServer((_req, res) => {
    res.writeHead(200, { 'Content-Type': 'application/json' }); res.flushHeaders();
    const first = setTimeout(() => res.write('{"ok":'), 9000);
    const last = setTimeout(() => res.end('true}'), 18000);
    res.on('close', () => { clearTimeout(first); clearTimeout(last); });
  });
  const port = await listen(server, t);
  assert.equal((await probe({ url: `http://127.0.0.1:${port}` })).body.ok, true);
});

test('real HTTPS transport validates both the CA bundle and hostname', async t => {
  const root = dir(t), key = join(root, 'key.pem'), cert = join(root, 'cert.pem');
  execFileSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-out', cert, '-days', '1', '-subj', '/CN=fixture.invalid', '-addext', 'subjectAltName=IP:127.0.0.1'], { stdio: 'ignore' });
  const server = createHttpsServer({ key: readFileSync(key), cert: readFileSync(cert) }, (_req, res) => res.end('{"ok":true}'));
  const port = await listen(server, t);
  assert.equal((await probe({ url: `https://127.0.0.1:${port}` })).failed, true);
  const env = { ...process.env, SSL_CERT_FILE: cert }; assert.equal((await probe({ url: `https://127.0.0.1:${port}` }, env)).body.ok, true);
  assert.equal((await probe({ url: `https://localhost:${port}` }, env)).failed, true);
});
