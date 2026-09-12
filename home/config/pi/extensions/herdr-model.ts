import { createConnection } from 'node:net';

// Companion to Herdr's managed lifecycle extension. Presentation only.
const source = 'user:pi-model';
const ttlMs = 15000;

function clean(value: unknown): string {
  return typeof value === 'string'
    ? value.replace(/[\u0000-\u001f\u007f-\u009f]/g, '').trim()
    : '';
}

export function displayLabel(ctx: { thinkingLevel?: string; model?: { id?: string } }, env: NodeJS.Dict<string> = {}) {
  const role = env.PI_TEAM_CHILD === '1' ? clean(env.PI_TEAM_ROLE) : '';
  const label = `pi · ${role || clean(ctx.thinkingLevel) || '?'} · ${clean(ctx.model?.id) || 'model unknown'}`;
  const chars = [...label];
  return chars.length <= 80 ? label : `${chars.slice(0, 79).join('')}…`;
}

export function sendMetadata(params: Record<string, unknown>, env: NodeJS.ProcessEnv = process.env) {
  return new Promise((resolve) => {
    const socket = createConnection(env.HERDR_SOCKET_PATH);
    let timer;
    const finish = () => {
      clearTimeout(timer);
      socket.destroy();
      resolve();
    };
    socket.on('error', finish);
    socket.on('end', finish);
    socket.on('data', finish);
    socket.on('connect', () => socket.write(`${JSON.stringify({
      id: `${source}:${params.seq}`,
      method: 'pane.report_metadata',
      params: { ...params, pane_id: env.HERDR_PANE_ID },
    })}\n`));
    timer = setTimeout(finish, 500);
    timer.unref?.();
  });
}

export default function herdrModel(pi: { on: (name: string, handler: (...args: any[]) => unknown) => void }, options: {
  env?: NodeJS.ProcessEnv;
  send?: (params: Record<string, unknown>) => Promise<void>;
  every?: typeof setInterval;
  cancel?: typeof clearInterval;
} = {}) {
  const env = options.env ?? process.env;
  if (env.HERDR_ENV !== '1' || !env.HERDR_SOCKET_PATH || !env.HERDR_PANE_ID) return;
  const send = options.send ?? ((params) => sendMetadata(params, env));
  const every = options.every ?? setInterval;
  const cancel = options.cancel ?? clearInterval;
  let active = false;
  let timer;
  let label;
  let sequence = Date.now() * 1000;

  function report(clear = false) {
    return send({
      source,
      agent: 'pi',
      seq: ++sequence,
      ttl_ms: ttlMs,
      ...(clear ? { clear_display_agent: true } : { display_agent: label }),
    }).catch(() => {}); // A stopped Herdr server must not interrupt the agent.
  }

  async function update(_event, ctx) {
    if (!active) return;
    label = displayLabel(ctx, env);
    await report();
  }

  pi.on('session_start', async (event, ctx) => {
    // RPC and headless subagents can inherit the parent's pane environment.
    if (ctx.mode !== 'tui') return;
    if (timer) cancel(timer);
    active = true;
    await update(event, ctx);
    timer = every(() => report(), 5000);
    timer.unref?.();
  });
  pi.on('model_select', update);
  pi.on('thinking_level_select', update);
  pi.on('agent_start', update);
  pi.on('session_tree', update);
  pi.on('session_shutdown', async () => {
    if (!active) return;
    active = false;
    if (timer) cancel(timer);
    timer = undefined;
    await report(true);
  });
}
