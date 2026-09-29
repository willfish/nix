import assert from 'node:assert/strict';
import test from 'node:test';
import { observeQwenMessage, type QwenLoopState } from '../home/config/local-llm/pi-qwen.ts';

const summary = 'All good here.\n\n## Summary\nNothing is pending.';

function assistant(text: string, tools: Array<{ name: string; arguments: Record<string, unknown> }> = []) {
  return {
    role: 'assistant',
    provider: 'andromeda',
    stopReason: tools.length ? 'toolUse' : 'stop',
    content: [
      { type: 'thinking', thinking: '## Summary\nignore thinking' },
      { type: 'text', text },
      ...tools.map((tool) => ({ type: 'toolCall', ...tool })),
    ],
  };
}

test('a finished summary drops the trailing tool calls', () => {
  const observed = observeQwenMessage(assistant(summary, [
    { name: 'bash', arguments: { command: 'git status' } },
    { name: 'question', arguments: { question: "How's it going?" } },
  ]));

  assert.equal(observed.replacement?.stopReason, 'stop');
  assert.deepEqual(
    observed.replacement?.content?.map((block: { type?: string }) => block.type),
    ['thinking', 'text'],
  );
});

test('a tool call before the answer is left alone', () => {
  const message = assistant('I will check the working tree.', [
    { name: 'bash', arguments: { command: 'git status' } },
  ]);
  const observed = observeQwenMessage(message);
  assert.equal(observed.replacement, undefined);
});

test('a question that only repeats the user is dropped', () => {
  let state: QwenLoopState = {};
  state = observeQwenMessage({ role: 'user', content: "How's it going?" }, state).state;
  const observed = observeQwenMessage(assistant('Checking.', [
    { name: 'bash', arguments: { command: 'git status' } },
    { name: 'question', arguments: { question: "How's it going?" } },
  ]), state);

  assert.equal(observed.replacement?.stopReason, 'toolUse');
  assert.deepEqual(
    observed.replacement?.content?.filter((block: { type?: string }) => block.type === 'toolCall'),
    [{ type: 'toolCall', name: 'bash', arguments: { command: 'git status' } }],
  );
});

test('the same finished answer and tool call does not run twice', () => {
  const message = assistant('The working tree is clean and there is nothing else to inspect here.', [
    { name: 'bash', arguments: { command: 'git status' } },
  ]);
  const first = observeQwenMessage(message);
  const second = observeQwenMessage(message, first.state);

  assert.equal(first.replacement, undefined);
  assert.equal(second.replacement?.stopReason, 'stop');
  assert.equal(second.replacement?.content?.some((block: { type?: string }) => block.type === 'toolCall'), false);
});

test('a new user message allows the same tool again', () => {
  const message = assistant('The working tree is clean and there is nothing else to inspect here.', [
    { name: 'bash', arguments: { command: 'git status' } },
  ]);
  const first = observeQwenMessage(message);
  const reset = observeQwenMessage({ role: 'user', content: 'Check again' }, first.state);
  const second = observeQwenMessage(message, reset.state);

  assert.equal(second.replacement, undefined);
});
