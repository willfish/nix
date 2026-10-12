// Same filter as Pi's getSupportedThinkingLevels. Keep this in lockstep with
// the installed agent: null hides a level, and xhigh/max appear only when mapped.
export const THINKING_LEVELS = ['off', 'minimal', 'low', 'medium', 'high', 'xhigh', 'max'] as const;

export type ThinkingLevel = (typeof THINKING_LEVELS)[number];

export const LEVEL_DESCRIPTIONS: Record<ThinkingLevel, string> = {
  off: 'No reasoning',
  minimal: 'Very brief reasoning (~1k tokens)',
  low: 'Light reasoning (~2k tokens)',
  medium: 'Moderate reasoning (~8k tokens)',
  high: 'Deep reasoning (~16k tokens)',
  xhigh: 'Extra-high reasoning (~32k tokens)',
  max: 'Maximum reasoning',
};

export type ThinkingModel = {
  reasoning?: boolean;
  thinkingLevelMap?: Partial<Record<ThinkingLevel, string | null>>;
};

export function supportedThinkingLevels(model: ThinkingModel | null | undefined): ThinkingLevel[] {
  if (!model?.reasoning) return ['off'];
  return THINKING_LEVELS.filter((level) => {
    const mapped = model.thinkingLevelMap?.[level];
    if (mapped === null) return false;
    if (level === 'xhigh' || level === 'max') return mapped !== undefined;
    return true;
  });
}

export function isThinkingLevel(value: string): value is ThinkingLevel {
  return (THINKING_LEVELS as readonly string[]).includes(value);
}
