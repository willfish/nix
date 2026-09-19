import assert from 'node:assert/strict';
import test from 'node:test';
import { readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

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
  assert.match(readme, /never asks it to rewrite/);
  assert.match(readme, /Do not add Jev to `~\/.pi\/agent\/models.json`/);
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
});
