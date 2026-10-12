import assert from 'node:assert/strict';
import test from 'node:test';
import { supportedThinkingLevels, THINKING_LEVELS } from './levels.ts';

test('models without reasoning only offer off', () => {
  assert.deepEqual(supportedThinkingLevels(undefined), ['off']);
  assert.deepEqual(supportedThinkingLevels({ reasoning: false }), ['off']);
});

test('reasoning models offer the standard levels unless a map hides them', () => {
  assert.deepEqual(supportedThinkingLevels({ reasoning: true }), [
    'off', 'minimal', 'low', 'medium', 'high',
  ]);
  assert.deepEqual(supportedThinkingLevels({
    reasoning: true,
    thinkingLevelMap: { minimal: null, low: null, xhigh: 'high', max: 'max' },
  }), ['off', 'medium', 'high', 'xhigh', 'max']);
});

test('virtual models expose only their declared levels', () => {
  const declared = ['low', 'high'];
  const thinkingLevelMap = Object.fromEntries(
    THINKING_LEVELS.map((level) => [level, declared.includes(level) ? level : null]),
  );
  assert.deepEqual(supportedThinkingLevels({ reasoning: true, thinkingLevelMap }), ['low', 'high']);
});
