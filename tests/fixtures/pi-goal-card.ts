// Offline TUI fixture: real goal/card lifecycle, deterministic auditor, no model calls.
import { writeFileSync } from 'node:fs';
import { Text, visibleWidth } from '@earendil-works/pi-tui';
import { keyHint } from '@earendil-works/pi-coding-agent';

export default async function(pi) {
  const { createGoalExtension, createAuditCardComponent } = await import(process.env.PI_GOAL_CARD_SOURCE);
  let finish;
  createGoalExtension({
    runAudit: ({ contract, onProgress, signal }) => new Promise(resolve => {
      onProgress({ id: 'first', tool: 'read', path: 'HISTORY_ONLY.ts', state: 'done' });
      onProgress({ id: 'second', tool: 'grep', path: 'CURRENT_PATH.ts', state: 'running' });
      signal.addEventListener('abort', () => resolve({ error: 'cancelled' }), { once: true });
      finish = () => resolve({ text: JSON.stringify({ auditId: contract.auditId, goalId: contract.id,
        revision: contract.revision, verdict: 'PASS', requirements: contract.requirements.map(item => ({
          id: item.id, status: 'verified', evidence: ['EVIDENCE_DETAIL_ONLY: source independently inspected'],
        })),
      }) });
    }),
    renderCard(getCard, expanded, theme) {
      const component = createAuditCardComponent(getCard, expanded, theme, { Text, keyHint });
      const mouse = component.handleMouse;
      component.handleMouse = event => {
        const result = mouse(event);
        if (result?.handled) writeFileSync('clicked.txt', 'clicked');
        return result;
      };
      const render = component.render;
      component.render = width => {
        const lines = render(width);
        if (lines.some(line => visibleWidth(line) > width)) throw Error('Audit card exceeded terminal width');
        writeFileSync('frame.json', JSON.stringify({ width, lines }));
        return lines;
      };
      return component;
    },
  })({ ...pi, sendMessage() {} }); // Deliberately suppress implementation model turns.
  pi.registerCommand('fixture-finish', { handler: async () => finish?.() });
  pi.on('input', () => ({ action: 'handled' }));
}
