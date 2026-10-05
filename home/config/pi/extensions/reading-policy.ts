export const CORE_DOC_TOPICS = '- When asked about: extensions (docs/extensions.md, examples/extensions/), themes (docs/themes.md), skills (docs/skills.md), prompt templates (docs/prompt-templates.md), TUI components (docs/tui.md), keybindings (docs/keybindings.md), SDK integrations (docs/sdk.md), custom providers (docs/custom-provider.md), adding models (docs/models.md), pi packages (docs/packages.md), environment variables (docs/environment-variables.md)';
export const CORE_READING_RULE = '- When working on pi topics, read the docs and examples, and follow .md cross-references before implementing\n- Always read pi .md files completely and follow links to related docs (e.g., tui.md for TUI API details)';
export const RELEVANT_READING_RULE = '- Before implementing Pi changes, read the relevant installed documentation sections and examples, including API dependencies and task-relevant cross-references. Read whole files only when needed for correctness; do not follow unrelated links.';

export function replaceReadingPolicy(prompt: string, options?: {
  customPrompt?: unknown;
  forceSystemPrompt?: string;
  sections?: Record<string, string>;
  appendSystemPrompt?: string;
  contextFiles?: { content?: string }[];
}) {
  if (!options || options.customPrompt || !prompt.startsWith('You are an expert coding assistant operating inside pi, a coding agent harness.') || prompt.split(CORE_READING_RULE).length !== 2) return prompt;
  const index = prompt.indexOf(CORE_READING_RULE);
  const header = 'Pi documentation (read only when the user asks about pi itself, its SDK, extensions, themes, skills, or TUI):';
  const headerIndex = prompt.indexOf(header);
  if (headerIndex < 0 || headerIndex >= index) return prompt;
  const docLines = prompt.slice(headerIndex + header.length, index).trim().split('\n');
  const topics = docLines.at(-1);
  if (docLines.some(line => !line.startsWith('- ')) || !(topics === CORE_DOC_TOPICS || topics?.startsWith(`${CORE_DOC_TOPICS}, `))) return prompt;
  if (options.sections?.docs !== undefined) return prompt;
  const contributed = [options.appendSystemPrompt, ...Object.values(options.sections ?? {}), ...(options.contextFiles ?? []).map(file => file.content)];
  if (contributed.some(text => text?.includes(CORE_READING_RULE))) return prompt;
  return prompt.slice(0, index) + RELEVANT_READING_RULE + prompt.slice(index + CORE_READING_RULE.length);
}

export default function readingPolicy(pi: {
  on: (name: string, handler: (event: { systemPrompt: string; systemPromptOptions?: Parameters<typeof replaceReadingPolicy>[1] }) => unknown) => void;
}) {
  pi.on('before_agent_start', event => {
    const systemPrompt = replaceReadingPolicy(event.systemPrompt, event.systemPromptOptions);
    if (systemPrompt !== event.systemPrompt) {
      const options = event.systemPromptOptions;
      const docs = systemPrompt.match(/<docs>\n([\s\S]*?)\n<\/docs>/);
      if (options?.sections && options.forceSystemPrompt === undefined && docs) {
        options.sections.docs = docs[1];
      } else {
        return { systemPrompt };
      }
    }
  });
}
