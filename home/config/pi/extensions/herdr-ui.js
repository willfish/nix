// Bridge Pi's dialog lifecycle to the event consumed by Herdr's integration.
// Pi coalesces nested dialogs into one outer waiting span.
export default function (pi) {
  pi.on('ui_prompt_start', (event, ctx) => {
    if (ctx.mode !== 'tui') return;
    pi.events.emit('herdr:blocked', {
      active: true,
      label: event.title || 'Waiting for input',
    });
  });
  pi.on('ui_prompt_end', (_event, ctx) => {
    if (ctx.mode !== 'tui') return;
    pi.events.emit('herdr:blocked', { active: false });
  });
}
