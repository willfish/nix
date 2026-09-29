export type QwenLoopState = {
  lastUserText?: string;
  previousText?: string;
  previousTools?: string[];
};

type QwenContentBlock = {
  type?: string;
  text?: string;
  name?: string;
  arguments?: unknown;
};

type QwenMessage = {
  role?: string;
  content?: unknown;
  stopReason?: string;
  provider?: string;
};

type QwenPi = {
  on: (name: string, handler: (event: any, ctx?: { model?: { provider?: string } }) => unknown) => void;
  getThinkingLevel: () => string;
};

const LOCAL_QWEN_PROVIDERS = new Set(["andromeda", "relay"]);
const FINAL_SUMMARY = /(?:^|\n)#{1,3} (?:Summary|TL;DR)\b/;

export function isLocalQwenProvider(provider: string | undefined): boolean {
  return provider !== undefined && LOCAL_QWEN_PROVIDERS.has(provider);
}

export function visibleText(content: unknown): string {
  if (typeof content === "string") return content;
  if (!Array.isArray(content)) return "";
  return content
    .filter((block): block is QwenContentBlock => Boolean(block) && typeof block === "object")
    .filter((block) => block.type === "text" && typeof block.text === "string")
    .map((block) => block.text as string)
    .join("\n");
}

function toolBlocks(content: unknown): QwenContentBlock[] {
  if (!Array.isArray(content)) return [];
  return content.filter(
    (block): block is QwenContentBlock =>
      Boolean(block) && typeof block === "object" && block.type === "toolCall",
  );
}

function toolKey(block: QwenContentBlock): string {
  return JSON.stringify({ name: block.name, arguments: block.arguments ?? {} });
}

function isQuestionEcho(block: QwenContentBlock, lastUserText: string | undefined): boolean {
  if (block.name !== "question" || !lastUserText) return false;
  const args = block.arguments;
  if (!args || typeof args !== "object") return false;
  const asked = (args as { question?: unknown }).question;
  return typeof asked === "string" && asked.trim() === lastUserText.trim();
}

export function observeQwenMessage(
  message: QwenMessage,
  state: QwenLoopState = {},
): { state: QwenLoopState; replacement?: QwenMessage } {
  if (message.role === "user") {
    return { state: { lastUserText: visibleText(message.content).trim() } };
  }
  if (message.role !== "assistant" || !Array.isArray(message.content)) {
    return { state };
  }

  const text = visibleText(message.content).trim();
  const calls = toolBlocks(message.content);
  if (calls.length === 0) {
    return { state: { ...state, previousText: text, previousTools: [] } };
  }

  const answered = FINAL_SUMMARY.test(text);
  const repeatedAnswer =
    state.previousText !== undefined &&
    state.previousText.length >= 40 &&
    state.previousText === text;
  const previousTools = new Set(state.previousTools ?? []);
  const kept = answered
    ? []
    : calls.filter((call) => {
        if (isQuestionEcho(call, state.lastUserText)) return false;
        return !(repeatedAnswer && previousTools.has(toolKey(call)));
      });

  const nextState = {
    ...state,
    previousText: text,
    previousTools: calls.map(toolKey),
  };
  if (kept.length === calls.length) return { state: nextState };

  const dropped = new Set(calls.filter((call) => !kept.includes(call)));
  return {
    state: nextState,
    replacement: {
      ...message,
      stopReason: kept.length === 0 && message.stopReason === "toolUse" ? "stop" : message.stopReason,
      content: message.content.filter((block) => !dropped.has(block as QwenContentBlock)),
    },
  };
}

// Pi handles enable_thinking; Qwen 3.8 also needs its level inside the template kwargs.
// A finished local answer can still emit tool calls. Drop those before Pi continues the turn.
export default function localQwen(pi: QwenPi) {
  let loopState: QwenLoopState = {};

  pi.on("message_end", (event, ctx) => {
    const message = event?.message as QwenMessage | undefined;
    if (!message) return;
    const provider = message.provider ?? ctx?.model?.provider;
    if (!isLocalQwenProvider(provider)) return;
    const observed = observeQwenMessage(message, loopState);
    loopState = observed.state;
    return observed.replacement ? { message: observed.replacement } : undefined;
  });

  pi.on("before_provider_request", (event, ctx) => {
    if (!isLocalQwenProvider(ctx?.model?.provider)) return;
    const payload = event.payload;
    const thinking = payload.chat_template_kwargs?.enable_thinking === true;
    const level = pi.getThinkingLevel();
    const effort = level === "high" || level === "xhigh" || level === "max" ? "xhigh"
      : level === "medium" ? "medium" : "low";
    return {
      ...payload,
      chat_template_kwargs: {
        ...payload.chat_template_kwargs,
        enable_thinking: thinking,
        preserve_thinking: true,
        ...(thinking ? { reasoning_effort: effort } : {}),
      },
      temperature: thinking ? 1.0 : 0.7,
      top_p: thinking ? 0.95 : 0.8,
      top_k: 20,
      min_p: 0,
      presence_penalty: thinking ? 0 : 1.5,
      repeat_penalty: 1,
    };
  });
}
