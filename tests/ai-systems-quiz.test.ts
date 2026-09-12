import assert from 'node:assert/strict';
import { readdirSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import {
  LESSONS, applyGrade, bindLesson, countHits, gradeChoice, gradeDraft, lessonIdFrom,
} from '../home/config/llm/learning/ai-systems-engineering/quiz.ts';

const lessonsDir = join(dirname(fileURLToPath(import.meta.url)), '../home/config/llm/learning/ai-systems-engineering/lessons');

function fakeNode(id, extras = {}) {
  const node = {
    id, className: '', textContent: '', value: extras.value ?? '',
    listeners: {},
    classList: { add(name) { node.className = `${node.className} ${name}`.trim(); } },
    addEventListener(name, handler) { node.listeners[name] = handler; },
    click() { node.listeners.click?.(); },
    ...extras,
  };
  return node;
}

test('choice grading distinguishes empty, correct and incorrect answers', () => {
  const quiz = LESSONS['0001'].quiz;
  assert.deepEqual(gradeChoice(undefined, quiz), { band: 'warn', text: quiz.empty });
  assert.equal(gradeChoice('b', quiz).band, 'good');
  assert.equal(gradeChoice('a', quiz).band, 'bad');
  assert.equal(gradeChoice('c', quiz).band, 'bad');
});

test('draft scoring uses the original needles and thresholds', () => {
  assert.equal(countHits('Tools and state', ['tool', 'state', 'eval']), 2);
  const weak = gradeDraft('I built an agent', LESSONS['0002'].draft);
  assert.equal(weak.band, 'bad');
  assert.match(weak.text, /0\/8 workflow concerns present/);
  const mid = gradeDraft('node state tool interrupt resume', LESSONS['0002'].draft);
  assert.equal(mid.band, 'warn');
  const strong = gradeDraft('node state tool interrupt resume failure approval output', LESSONS['0002'].draft);
  assert.equal(strong.band, 'good');
  assert.equal(gradeDraft('x', LESSONS['0004'].draft).band, 'bad');
  assert.equal(gradeDraft('input expected tool guardrail grader', LESSONS['0004'].draft).band, 'warn');
  assert.equal(gradeDraft('problem architecture workflow tools state eval guardrail human fallback trace metric cost tradeoff cv', LESSONS['0006'].draft).band, 'good');
});

test('lesson ids come from explicit data or the html filename', () => {
  assert.equal(lessonIdFrom('/tmp/0003-design-tool-and-mcp-boundaries.html'), '0003');
  assert.equal(lessonIdFrom('/tmp/unknown.html', '0005'), '0005');
  assert.equal(lessonIdFrom('/tmp/unknown.html'), undefined);
});

test('DOM binding scores drafts, quizzes and resets without inline page scripts', () => {
  const onepager = fakeNode('onepager', { value: 'tools state evals guardrails human observability latency fallbacks portfolio' });
  const score = fakeNode('score');
  const scoreResult = fakeNode('score-result');
  const check = fakeNode('check-quiz');
  const quizResult = fakeNode('quiz-result');
  const reset = fakeNode('reset');
  const chosen = { value: 'b' };
  const doc = {
    querySelector: () => chosen,
    getElementById: (id) => ({
      onepager, score, 'score-result': scoreResult, 'check-quiz': check, 'quiz-result': quizResult, reset,
    }[id] ?? null),
  };
  bindLesson(LESSONS['0001'], doc);
  score.click();
  assert.match(scoreResult.className, /good/);
  assert.match(scoreResult.textContent, /Phase 3 shape/);
  check.click();
  assert.match(quizResult.className, /good/);
  reset.click();
  assert.equal(onepager.value, LESSONS['0001'].template);
  assert.match(scoreResult.textContent, /Template reset/);
});

test('applyGrade replaces previous result classes', () => {
  const node = fakeNode('result');
  applyGrade(node, { band: 'warn', text: 'hold' });
  applyGrade(node, { band: 'good', text: 'ok' });
  assert.equal(node.className, 'result good');
  assert.equal(node.textContent, 'ok');
});

test('every lesson page loads the TypeScript quiz module and has no inline JavaScript', () => {
  const files = readdirSync(lessonsDir).filter((name) => name.endsWith('.html'));
  assert.equal(files.length, 6);
  for (const name of files) {
    const html = readFileSync(join(lessonsDir, name), 'utf8');
    const id = name.slice(0, 4);
    assert.match(html, new RegExp(`<html lang="en" data-lesson="${id}">`));
    assert.match(html, /<script type="module" src="\.\.\/quiz\.ts"><\/script>/);
    assert.doesNotMatch(html, /<script>/);
    assert.ok(LESSONS[id], `missing lesson config ${id}`);
  }
});
