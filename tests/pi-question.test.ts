import assert from 'node:assert/strict';
import test from 'node:test';
import questionExtension, { isQuestionParent, OTHER_ANSWER, promptWithChoices } from '../home/config/pi/extensions/question.ts';

function harness({ child = false, mode = 'tui' } = {}) {
  const tools = new Map(), events = {}, uiCalls = [];
  const env = child ? { PI_TEAM_CHILD: '1' } : {};
  const ui = {
    select: async (title, choices, options) => { uiCalls.push({ method: 'select', title, choices, options }); return choices[0]; },
    input: async (title, _placeholder, options) => { uiCalls.push({ method: 'input', title, options }); return 'typed'; },
  };
  const pi = {
    registerTool: tool => tools.set(tool.name, tool),
    on: (name, handler) => { events[name] = handler; },
  };
  const prev = process.env.PI_TEAM_CHILD;
  if (child) process.env.PI_TEAM_CHILD = '1';
  else delete process.env.PI_TEAM_CHILD;
  try { questionExtension(pi); }
  finally {
    if (prev === undefined) delete process.env.PI_TEAM_CHILD;
    else process.env.PI_TEAM_CHILD = prev;
  }
  return { tools, events, uiCalls, env, ctx: { mode, ui },
    execute: (params, signal) => tools.get('question').execute('q1', params, signal, undefined, { mode, ui }) };
}

test('parent process loads the question tool; team children do not', () => {
  assert.equal(isQuestionParent({}), true);
  assert.equal(isQuestionParent({ PI_TEAM_CHILD: '1' }), false);
  const parent = harness();
  assert.equal(parent.tools.has('question'), true);
  assert.match((parent.events.before_agent_start({ systemPrompt: 'base' })).systemPrompt, /question tool/);
  assert.equal(harness({ child: true }).tools.has('question'), false);
});

test('question tool requires two options and uses a selectable list', async () => {
  const h = harness();
  await assert.rejects(h.execute({ question: 'Go?', options: ['only'] }), /at least two options/);
  const result = await h.execute({ question: 'Deploy?', options: ['staging', 'production'] });
  assert.equal(result.details.answer, 'staging');
  assert.deepEqual(h.uiCalls[0].choices, ['staging', 'production', OTHER_ANSWER]);
  assert.equal(h.uiCalls[0].method, 'select');
});

test('other answer falls through to typed input; cancel leaves no answer', async () => {
  const h = harness();
  h.ctx.ui.select = async (_title, choices) => choices.at(-1);
  h.ctx.ui.input = async () => '  custom  ';
  const result = await h.execute({ question: 'Deploy?', options: ['staging', 'production'] });
  assert.equal(result.details.answer, 'custom');
  assert.match(result.content[0].text, /custom/);
  const cancelled = harness();
  cancelled.ctx.ui.select = async () => undefined;
  const none = await cancelled.execute({ question: 'Deploy?', options: ['staging', 'production'] });
  assert.equal(none.details.answer, null);
  await assert.rejects(harness({ mode: 'rpc' }).execute({ question: 'Deploy?', options: ['a', 'b'] }), /interactive/);
});

test('promptWithChoices refuses questions without a selectable list', async () => {
  const ui = { select: async () => 'nope', input: async (title) => `typed:${title}` };
  await assert.rejects(promptWithChoices(ui, 'Choose', undefined, undefined), /at least two concrete options/);
  await assert.rejects(promptWithChoices(ui, 'Choose', ['only'], undefined), /at least two concrete options/);
});
