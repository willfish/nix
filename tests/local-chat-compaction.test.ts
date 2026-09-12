import assert from 'node:assert/strict';
import test from 'node:test';
import { compactRequest } from '../home/config/local-llm/compaction.ts';

const message = (role, content) => ({ role, content });
const history = (n = 20) => Array.from({ length: n }, (_, i) => [
  message('user', `Question ${i}: ${'detail '.repeat(60)}`),
  message('assistant', `Answer ${i}: ${'finding '.repeat(60)}`),
]).flat();
const request = (messages = history()) => ({ model: 'model-a', messages, tools: [] });
const count = async (body) => Math.ceil(JSON.stringify(body).length / 4);
function harness(overrides = {}) {
  const state = { cache: undefined, calls: [], saves: 0 };
  const options = {
    contextSize: 4096, identity: 'model-a-template', count,
    load: async () => state.cache,
    save: async (entry) => { state.cache = entry; state.saves++; },
    summarise: async (body) => { state.calls.push(body); return 'Continuity summary: retain the task and findings.'; },
    ...overrides,
  };
  return { state, options };
}

test('short requests pass through without summarisation', async () => {
  const h = harness();
  const input = request([message('user', 'Hello')]);
  assert.deepEqual(await compactRequest(input, h.options), input);
  assert.equal(h.state.calls.length, 0);
});

test('summary size accounting works with templates that reject assistant-only input', async () => {
  const h = harness({ count: async body => {
    if (body.messages.length === 1 && body.messages[0].role === 'assistant') {
      throw Error('No messages provided: assistant prefill removed the only message.');
    }
    return count(body);
  } });
  await compactRequest(request(), h.options);
  assert.equal(h.state.saves, 1);
});

test('compacts above 75%, preserving original history and current request', async () => {
  const h = harness();
  const input = request([...history(), message('user', 'Now finish the original task.')]);
  const before = JSON.stringify(input);
  const result = await compactRequest(input, h.options);
  assert.ok(h.state.calls.length > 0);
  assert.ok(await count(result) <= 3072);
  assert.equal(JSON.stringify(input), before);
  assert.deepEqual(result.messages.at(-1), input.messages.at(-1));
  assert.equal(h.state.saves, 1);
});

test('preserves system/developer messages and schemas verbatim, summary is not privileged', async () => {
  const h = harness();
  const instructions = [message('system', 'Never alter files.'), message('developer', 'Use read-only tools.')];
  const input = request([...instructions, ...history()]);
  input.tools = [{ type: 'function', function: { name: 'read', parameters: { type: 'object' } } }];
  const result = await compactRequest(input, h.options);
  assert.deepEqual(result.messages.slice(0, 2), instructions);
  assert.equal(result.messages[2].role, 'assistant');
  assert.deepEqual(result.tools, input.tools);
});

test('reuses a persistent summary after appending history without regenerating it', async () => {
  const h = harness();
  const input = request();
  await compactRequest(input, h.options);
  const calls = h.state.calls.length;
  const result = await compactRequest(request([...input.messages, message('user', 'Continue.')]), h.options);
  assert.equal(h.state.calls.length, calls);
  assert.ok(result.messages.some(m => m.content.includes('Continuity summary')));
});

test('repeated compaction summarises the previous note plus newly covered messages', async () => {
  const h = harness();
  const input = request();
  await compactRequest(input, h.options);
  const calls = h.state.calls.length;
  const covered = h.state.cache.covered;
  const result = await compactRequest(request([...input.messages, ...history(25)]), h.options);
  assert.ok(h.state.calls.length > calls);
  assert.ok(h.state.cache.covered > covered);
  assert.ok(JSON.stringify(h.state.calls[calls]).includes('Continuity summary'));
  assert.ok(await count(result) <= 3072);
});

test('edits to covered history invalidate the summary', async () => {
  const h = harness();
  const input = request();
  await compactRequest(input, h.options);
  const calls = h.state.calls.length;
  input.messages[0].content = 'A different goal';
  await compactRequest(input, h.options);
  assert.ok(h.state.calls.length > calls);
  assert.ok(h.state.cache.prefix.includes('A different goal'));
});

test('model and instruction changes invalidate the summary', async () => {
  const h = harness();
  await compactRequest(request(), h.options);
  const calls = h.state.calls.length;
  await compactRequest(request([message('system', 'New instructions'), ...history()]), {
    ...h.options, identity: 'model-b-template',
  });
  assert.ok(h.state.calls.length > calls);
});

test('larger context automatically permits more history', async () => {
  const h = harness({ contextSize: 16384 });
  const input = request();
  assert.deepEqual(await compactRequest(input, h.options), input);
  assert.equal(h.state.calls.length, 0);
});

test('explicit output budget triggers compaction earlier', async () => {
  const h = harness();
  const input = { ...request(history(10)), max_tokens: 2200 };
  const result = await compactRequest(input, h.options);
  assert.ok(h.state.calls.length > 0);
  assert.ok(await count(result) + 2200 < 4096);
});

test('tool batches stay intact and latest user request survives a long tool loop', async () => {
  const h = harness();
  const goal = message('user', 'Investigate every item.');
  const toolHistory = Array.from({ length: 25 }, (_, i) => [
    { role: 'assistant', content: '', tool_calls: [{ id: `a${i}` }, { id: `b${i}` }] },
    { role: 'tool', tool_call_id: `a${i}`, content: 'tool data '.repeat(60) },
    { role: 'tool', tool_call_id: `b${i}`, content: 'more data '.repeat(60) },
  ]).flat();
  const result = await compactRequest(request([goal, ...toolHistory]), h.options);
  assert.ok(result.messages.some(m => m.content === goal.content));
  const calls = new Set(result.messages.flatMap(m => (m.tool_calls ?? []).map(c => c.id)));
  const results = new Set(result.messages.filter(m => m.role === 'tool').map(m => m.tool_call_id));
  assert.deepEqual(results, calls);
  assert.ok(h.state.cache.covered % 3 === 1);
});

test('cannot silently drop a giant current request or tool result', async () => {
  const h = harness();
  await assert.rejects(compactRequest(request([message('user', 'x'.repeat(30000))]), h.options), /cannot fit|too large/i);
  assert.equal(h.state.saves, 0);
});

test('rejects incomplete tool batches rather than orphaning results', async () => {
  const h = harness();
  await assert.rejects(compactRequest(request([
    ...history(), { role: 'assistant', tool_calls: [{ id: 'missing' }] },
  ]), h.options), /tool/i);
});

test('chunks very long old transcripts into context-safe summary requests', async () => {
  const h = harness();
  await compactRequest(request(history(80)), h.options);
  assert.ok(h.state.calls.length > 1);
  for (const body of h.state.calls) assert.ok(await count(body) + body.max_tokens < 4096);
});

test('summary failures leave input and prior cache intact', async () => {
  const h = harness({ summarise: async () => { throw Error('summary failed'); } });
  const input = request();
  const original = JSON.stringify(input);
  await assert.rejects(compactRequest(input, h.options), /summary failed/);
  assert.equal(JSON.stringify(input), original);
  assert.equal(h.state.saves, 0);
});

test('empty summaries are rejected', async () => {
  const h = harness({ summarise: async () => '   ' });
  await assert.rejects(compactRequest(request(), h.options), /empty/i);
  assert.equal(h.state.saves, 0);
});

test('Stop cancels summarisation without committing a partial cache', async () => {
  const controller = new AbortController();
  const h = harness({ signal: controller.signal, summarise: async () => {
    controller.abort(); return 'Partial summary';
  } });
  await assert.rejects(compactRequest(request(), h.options), { name: 'AbortError' });
  assert.equal(h.state.saves, 0);
});

test('requires a real active context size rather than assuming 64K', async () => {
  const h = harness({ contextSize: undefined });
  await assert.rejects(compactRequest(request(), h.options), /context/i);
});
