import assert from 'node:assert/strict';
import test from 'node:test';
import { readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import {
  formatObservationLine,
  formatObservationMarkdown,
  htmlToMarkdown,
} from '../home/config/pi/extensions/pi-observational-memory-jev/ledger/html-markdown.ts';

const root = join(
  dirname(fileURLToPath(import.meta.url)),
  '..',
  'home/config/pi/extensions/pi-observational-memory-jev',
);

function read(rel: string): string {
  return readFileSync(join(root, rel), 'utf8');
}

test('pi-observational-memory-jev ships Jev observational memory with Alvar /om ergonomics', () => {
  const files = readdirSync(root);
  assert.ok(files.includes('index.ts'));
  assert.ok(files.includes('README.md'));
  assert.ok(readdirSync(join(root, 'jev')).includes('observe.ts'));
  assert.ok(readdirSync(join(root, 'ledger')).includes('fold.ts'));

  const readme = read('README.md');
  assert.match(readme, /pi-observational-memory-jev/);
  assert.match(readme, /never rewrites the transcript/);
  assert.match(readme, /models\.json/);
  assert.match(readme, /TYPESAFE_API_KEY/);
  assert.match(readme, /\/om on/);
  assert.match(readme, /\/om:status/);
  assert.match(readme, /\/om:compact/);
  assert.match(readme, /\/om:consolidate/);
  assert.match(readme, /amosblomqvist\/pi-observational-memory/);
  assert.match(readme, /tamaratran\/fast-jev-compaction/);

  const index = read('index.ts');
  assert.match(index, /registerCommand\("om"/);
  assert.match(index, /registerStatusCommand/);
  assert.match(index, /registerCompactCommand/);
  assert.match(index, /registerConsolidateCommand/);
  assert.match(index, /registerObserverTrigger/);
  assert.match(index, /registerCompactionHook/);

  assert.match(read('commands/status.ts'), /registerCommand\("om:status"/);
  assert.match(read('commands/compact.ts'), /registerCommand\("om:compact"/);
  assert.match(read('commands/consolidate.ts'), /registerCommand\("om:consolidate"/);

  const observe = read('jev/observe.ts');
  assert.match(observe, /keepThreshold/);
  assert.match(observe, /kind_/);
  assert.match(observe, /accepted\.push/);
  assert.match(observe, /content: source\.text/);

  const nix = readFileSync(join(root, '../../../../user/pi.nix'), 'utf8');
  assert.match(nix, /pi-observational-memory-jev/);
  assert.match(nix, /Not a chat model/);
  assert.match(nix, /TYPESAFE_API_KEY/);
  assert.match(nix, /config\.sops\.secrets\.TYPESAFE_API_KEY\.path/);

  const defaults = readFileSync(join(root, '../../settings-defaults.json'), 'utf8');
  assert.match(defaults, /observational-memory-jev/);
  assert.match(defaults, /enabledByDefault/);
});

test('html excerpts become markdown the compaction card can render', () => {
  assert.equal(
    htmlToMarkdown('## <span colorid="z0pxrn9bl1">“other” question responses</span>'),
    '## **“other” question responses**',
  );
  assert.equal(htmlToMarkdown('<span><strong>Metric</strong></span>'), '**Metric**');

  const table = htmlToMarkdown(
    '<table class="confluenceTable"><tbody><tr><th><p><strong>Metric</strong></p></th><th>Value</th></tr><tr><td>clicks</td><td>12</td></tr></tbody></table>',
  );
  assert.match(table, /\| \*\*Metric\*\* \| Value \|/);
  assert.match(table, /\| --- \| --- \|/);
  assert.match(table, /\| clicks \| 12 \|/);

  const content =
    '## <span colorid="z0pxrn9bl1">“other” question responses are leading to bad user experiences</span>';
  assert.equal(
    formatObservationMarkdown('2026-09-21T16:10:00.01', 'hypothesis', content),
    '### [hypothesis] · 2026-09-21T16:10:00.01\n\n## **“other” question responses are leading to bad user experiences**',
  );
  assert.equal(
    formatObservationLine('2026-09-21T16:10:00.01', 'hypothesis', content),
    '2026-09-21T16:10:00.01  [hypothesis] ## **“other” question responses are leading to bad user experiences**',
  );

  const render = read('ledger/render.ts');
  assert.match(render, /observationToMarkdown/);
  assert.match(render, /formatObservationMarkdown/);
  assert.match(render, /## Observations/);
});
