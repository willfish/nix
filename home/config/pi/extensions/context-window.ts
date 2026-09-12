// Session-only context budgets. Never mutate the shared model registry or settings.
// Astra's Codex catalogue advertises 272k default / 872k maximum (2026-09-09).
// These are local compaction ceilings, not a guarantee of backend acceptance.
const PROVIDER = 'openai-codex';
const MODEL = 'gpt-6-astra';
const ENTRY = 'context-window';
const PRESETS = [
  { name: 'lean', tokens: 272000, label: 'Lean: 272k (default)' },
  { name: 'extended', tokens: 500000, label: 'Extended: 500k (backend untested)' },
  { name: 'maximum', tokens: 872000, label: 'Maximum: 872k (backend untested)' },
];
const supported = (model?: { provider?: string; id?: string }) => model?.provider === PROVIDER && model.id === MODEL;

export default function (pi: {
  getThinkingLevel: () => string;
  setThinkingLevel: (level: string) => void;
  setModel: (model: unknown) => Promise<boolean>;
  registerCommand: (name: string, spec: Record<string, unknown>) => void;
  on: (name: string, handler: (...args: any[]) => unknown) => void;
}) {
  let applying = false;

  function status(ctx) {
    if (ctx.hasUI) {
      ctx.ui.setStatus(ENTRY, supported(ctx.model)
        ? `context: ${ctx.model.contextWindow / 1000}k`
        : undefined);
    }
  }

  async function apply(ctx, contextWindow) {
    if (ctx.model.contextWindow === contextWindow) {
      status(ctx);
      return true;
    }
    applying = true;
    const thinkingLevel = pi.getThinkingLevel();
    try {
      // setModel accepts a Model object. A copy preserves transport, output cap,
      // pricing and reasoning metadata without changing other/new sessions.
      if (!await pi.setModel({ ...ctx.model, contextWindow })) {
        if (ctx.hasUI) ctx.ui.notify('Context unchanged: provider authentication unavailable.', 'error');
        return false;
      }
      pi.setThinkingLevel(thinkingLevel);
      status(ctx);
      return true;
    } catch (error) {
      if (ctx.hasUI) ctx.ui.notify(`Context unchanged: ${error.message}`, 'error');
      return false;
    } finally {
      applying = false;
    }
  }

  async function restore(_event, ctx) {
    if (applying) return;
    if (!supported(ctx.model)) {
      status(ctx);
      return;
    }
    let tokens = PRESETS[0].tokens;
    for (const entry of ctx.sessionManager.getBranch()) {
      const data = entry.data;
      if (entry.type === 'custom' && entry.customType === ENTRY
        && data?.provider === PROVIDER && data.model === MODEL
        && PRESETS.some(preset => preset.tokens === data.contextWindow)) {
        tokens = data.contextWindow;
      }
    }
    await apply(ctx, tokens);
  }

  pi.on('session_start', restore);
  pi.on('session_tree', restore);
  pi.on('model_select', restore);
  // Pi emits no model_select when the same provider/id is reselected, even
  // though it replaces the model object. Restore before prompt compaction.
  pi.on('input', restore);

  pi.registerCommand('context', {
    description: 'Session context budget for Astra subscription: lean, extended, maximum',
    getArgumentCompletions(prefix) {
      const items = PRESETS.filter(preset => preset.name.startsWith(prefix))
        .map(preset => ({ value: preset.name, label: preset.label }));
      return items.length ? items : null;
    },
    async handler(args, ctx) {
      if (!ctx.hasUI) return;
      if (!supported(ctx.model)) {
        ctx.ui.notify('/context currently supports only openai-codex/gpt-6-astra.', 'warning');
        return;
      }
      if (applying || !ctx.isIdle()) {
        ctx.ui.notify('Wait until Pi is idle before changing context.', 'warning');
        return;
      }
      await restore(undefined, ctx);
      const model = ctx.model;
      const argument = args.trim().toLowerCase();
      let preset;
      if (argument) {
        preset = PRESETS.find(item => item.name === argument || `${item.tokens / 1000}k` === argument);
        if (!preset) {
          ctx.ui.notify('Usage: /context [lean|extended|maximum|272k|500k|872k]', 'warning');
          return;
        }
      } else {
        const labels = PRESETS.map(item => `${item.label}${item.tokens === model.contextWindow ? ' [current]' : ''}`);
        const selected = await ctx.ui.select('Session context budget', labels);
        preset = PRESETS[labels.indexOf(selected)];
        if (!preset) return;
      }
      if (preset.tokens === model.contextWindow) return;
      const tokens = ctx.getContextUsage()?.tokens;
      if (preset.tokens < model.contextWindow && (!Number.isFinite(tokens) || tokens >= preset.tokens)) {
        const usage = Number.isFinite(tokens) ? `${tokens} tokens` : 'unknown usage';
        if (!await ctx.ui.confirm('Reduce context budget?',
          `Current context: ${usage}. Reducing to ${preset.tokens / 1000}k may trigger lossy auto-compaction on the next turn. Continue?`)) return;
      }
      // Dialogs yield control. Do not apply a stale choice to a different model
      // or change limits underneath a running request.
      if (ctx.model !== model || applying || !ctx.isIdle()) {
        ctx.ui.notify('Session changed while choosing. Run /context again when idle.', 'warning');
        return;
      }
      if (!await apply(ctx, preset.tokens)) return;
      pi.appendEntry(ENTRY, { provider: PROVIDER, model: MODEL, contextWindow: preset.tokens });
      ctx.ui.notify(`Session context: ${preset.tokens / 1000}k. New sessions stay at 272k. `
        + 'Auto-compaction still reserves response space; increasing context cannot restore compacted details.'
        + (preset.tokens > 272000 ? ' Larger requests may use more subscription allowance; backend acceptance is untested.' : ''), 'info');
    },
  });
}
