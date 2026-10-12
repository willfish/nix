// Ctrl+T picks the thinking level for the current model.
// ui.custom replaces the editor, like the model selector, and does not mark the
// program blocked. ui.select would. setThinkingLevel applies on the next request
// and does not abort the current run.
import type { ExtensionAPI } from '@earendil-works/pi-coding-agent';
import { DynamicBorder } from '@earendil-works/pi-coding-agent';
import { Container, type SelectItem, SelectList, Text } from '@earendil-works/pi-tui';
import { isThinkingLevel, LEVEL_DESCRIPTIONS, supportedThinkingLevels, type ThinkingLevel } from './levels.ts';

const SHORTCUT = 'ctrl+t';

type PickerTheme = {
  fg: (token: string, text: string) => string;
  bold: (text: string) => string;
};

export function thinkingLevelItems(levels: ThinkingLevel[], current: string | undefined): SelectItem[] {
  return levels.map((level) => ({
    value: level,
    label: `${level === current ? '✓ ' : '  '}${level}`,
    description: LEVEL_DESCRIPTIONS[level],
  }));
}

export function applyThinkingLevel(pi: { getThinkingLevel: () => string; setThinkingLevel: (level: ThinkingLevel) => void }, level: string) {
  if (!isThinkingLevel(level) || pi.getThinkingLevel() === level) return false;
  pi.setThinkingLevel(level);
  return true;
}

function picker(theme: PickerTheme, items: SelectItem[], selected: number, done: (value: string | undefined) => void) {
  const container = new Container();
  container.addChild(new DynamicBorder((text: string) => theme.fg('accent', text)));
  container.addChild(new Text(theme.fg('accent', theme.bold('Thinking level')), 0, 0));
  const list = new SelectList(items, Math.max(1, items.length), {
    selectedPrefix: (text: string) => theme.fg('accent', text),
    selectedText: (text: string) => theme.fg('accent', text),
    description: (text: string) => theme.fg('muted', text),
    scrollInfo: (text: string) => theme.fg('dim', text),
    noMatch: (text: string) => theme.fg('warning', text),
  });
  if (selected >= 0) list.setSelectedIndex(selected);
  list.onSelect = (item: SelectItem) => done(item.value);
  list.onCancel = () => done(undefined);
  container.addChild(list);
  container.addChild(new Text(theme.fg('dim', '  enter selects  ·  esc cancels'), 0, 0));
  container.addChild(new DynamicBorder((text: string) => theme.fg('accent', text)));
  return {
    render: (width: number) => container.render(width),
    invalidate: () => container.invalidate(),
    // Container does not forward keys. The list owns selection.
    handleInput: (data: string) => list.handleInput(data),
  };
}

export default function thinkingLevelExtension(pi: ExtensionAPI) {
  let open = false;

  pi.registerShortcut(SHORTCUT, {
    description: 'Select thinking level',
    handler: async (ctx) => {
      if (open || !ctx.hasUI || ctx.mode !== 'tui') return;
      const levels = supportedThinkingLevels(ctx.model);
      if (levels.length === 0) {
        ctx.ui.notify('No thinking levels for this model', 'warning');
        return;
      }
      open = true;
      try {
        const current = ctx.thinkingLevel;
        const choice = await ctx.ui.custom<string | undefined>((tui, theme, _keybindings, done) => {
          const component = picker(theme, thinkingLevelItems(levels, current), levels.indexOf(current as ThinkingLevel), done);
          return {
            render: component.render,
            invalidate: component.invalidate,
            handleInput: (data: string) => {
              component.handleInput(data);
              tui.requestRender();
            },
          };
        });
        if (choice && applyThinkingLevel(pi, choice)) ctx.ui.notify(`Thinking level: ${choice}`, 'info');
      } finally {
        open = false;
      }
    },
  });
}
