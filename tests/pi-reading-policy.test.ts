import assert from 'node:assert/strict';
import test from 'node:test';
import readingPolicy, { CORE_DOC_TOPICS, CORE_READING_RULE, RELEVANT_READING_RULE, replaceReadingPolicy } from '../home/config/pi/extensions/reading-policy.ts';

const core = `You are an expert coding assistant operating inside pi, a coding agent harness.\nPi documentation (read only when the user asks about pi itself, its SDK, extensions, themes, skills, or TUI):\n- Main documentation: /installed/README.md\n${CORE_DOC_TOPICS}\n${CORE_READING_RULE}`;

test('exact core policy replacement preserves all other context', () => {
  const prompt = `${core}\n<project_context>approval and secrets</project_context>\nteam preload\nextension addition`;
  assert.equal(replaceReadingPolicy(prompt, {}), prompt.replace(CORE_READING_RULE, RELEVANT_READING_RULE));
  assert.match(RELEVANT_READING_RULE, /API dependencies/);
  assert.match(RELEVANT_READING_RULE, /before implementing/i);
});

test('version drift, custom prompts, missing options, ambiguous or contributed rules are no-ops', () => {
  for (const prompt of [core.replace('completely', 'in full'), 'unrelated ' + CORE_READING_RULE, core + CORE_READING_RULE, core.replace(CORE_READING_RULE, 'Changed core rule') + '\n\nextension\n' + CORE_READING_RULE, core.replace(CORE_READING_RULE, '- Changed core rule') + '\n' + CORE_READING_RULE]) {
    assert.equal(replaceReadingPolicy(prompt, {}), prompt);
  }
  for (const options of [undefined, { customPrompt: core }, { appendSystemPrompt: CORE_READING_RULE },
    { contextFiles: [{ content: CORE_READING_RULE }] }]) {
    assert.equal(replaceReadingPolicy(core, options), core);
  }
});

test('extension uses chained event prompt and returns no patch for drift', () => {
  const handlers = new Map();
  readingPolicy({ on: (name, handler) => handlers.set(name, handler) });
  const handler = handlers.get('before_agent_start');
  assert.deepEqual(handler({ systemPrompt: core + '\nprior extension', systemPromptOptions: {} }),
    { systemPrompt: core.replace(CORE_READING_RULE, RELEVANT_READING_RULE) + '\nprior extension' });
  assert.equal(handler({ systemPrompt: 'unknown', systemPromptOptions: {} }), undefined);
});
