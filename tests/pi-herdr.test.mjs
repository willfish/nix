import assert from 'node:assert/strict';
import test from 'node:test';
import install from '../home/config/pi/extensions/herdr-ui.js';

test('Pi UI waiting spans report a blocker and release it after dismissal', () => {
  const handlers = new Map();
  const reports = [];
  install({
    on: (name, handler) => handlers.set(name, handler),
    events: { emit: (name, data) => reports.push({ name, ...data }) },
  });
  handlers.get('ui_prompt_start')({ title: 'Approve command?', kind: 'confirm' }, { mode: 'tui' });
  handlers.get('ui_prompt_end')({}, { mode: 'tui' });
  assert.deepEqual(reports, [
    { name: 'herdr:blocked', active: true, label: 'Approve command?' },
    { name: 'herdr:blocked', active: false },
  ]);
});

test('headless prompts do not change a terminal agent status', () => {
  const handlers = new Map();
  install({
    on: (name, handler) => handlers.set(name, handler),
    events: { emit: () => assert.fail('headless session reported a blocker') },
  });
  for (const mode of ['rpc', 'print', 'json']) {
    handlers.get('ui_prompt_start')({ kind: 'custom' }, { mode });
    handlers.get('ui_prompt_end')({}, { mode });
  }
});
