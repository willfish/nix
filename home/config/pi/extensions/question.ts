// Parent-pane multiple-choice questions. Team children use ask_coordinator instead.
export const OTHER_ANSWER = 'Other answer (type text)';
const QUESTION_GUIDANCE = `When you must ask the human, call the question tool with a short question and 2-6 concrete options. Do not ask in the chat editor. Only ask for credentials, access, unapproved live/destructive work, or mandatory gates.`;

export function isQuestionParent(env = process.env) {
  return env.PI_TEAM_CHILD !== '1';
}

export async function promptWithChoices(ui, title, choices, signal) {
  if (choices?.length) {
    let text = await ui.select(title, [...choices, OTHER_ANSWER], { signal });
    if (text === OTHER_ANSWER && !signal?.aborted) text = await ui.input(title, undefined, { signal });
    return text;
  }
  return ui.input(title, undefined, { signal });
}

export default function questionExtension(pi) {
  if (!isQuestionParent()) return;
  pi.registerTool({
    name: 'question', label: 'Question',
    description: 'Ask the human a multiple-choice question in this pane. Always pass 2-6 concrete options. Only for credentials, access, unapproved live/destructive work, or mandatory gates. Never ask in prose.',
    parameters: {
      type: 'object', additionalProperties: false, required: ['question', 'options'],
      properties: {
        question: { type: 'string', minLength: 1 },
        options: { type: 'array', items: { type: 'string', minLength: 1 }, minItems: 2 },
      },
    },
    async execute(_id, params, signal, _onUpdate, ctx) {
      const question = typeof params.question === 'string' ? params.question.trim() : '';
      const options = Array.isArray(params.options)
        ? params.options.map(value => String(value).trim()).filter(Boolean) : [];
      if (!question || options.length < 2) throw new Error('Question requires text and at least two options');
      if (ctx.mode !== 'tui') throw new Error('Human answers require the main interactive Pi pane');
      const text = await promptWithChoices(ctx.ui, question, options, signal);
      if (signal?.aborted) throw new Error('Question cancelled');
      if (text === undefined || text === null || !String(text).trim()) {
        return { content: [{ type: 'text', text: 'User cancelled the selection' }], details: { question, options, answer: null } };
      }
      return { content: [{ type: 'text', text: `User selected: ${String(text).trim()}` }],
        details: { question, options, answer: String(text).trim() } };
    },
  });
  pi.on('before_agent_start', event => ({ systemPrompt: `${event.systemPrompt}\n\n${QUESTION_GUIDANCE}` }));
}
