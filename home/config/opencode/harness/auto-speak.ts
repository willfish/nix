import { speakText } from "./speak.ts";

export const VOICE_RPC_ID = "dotfiles.voice";
const PENSIVE = "[thoughtful] [calmly] ";

// Extract prose, not a second model-generated summary. Nothing runs while voice is off.
export function spokenSummary(text: string): string {
  const lines: string[] = [];
  let fence: string | undefined;
  for (const line of text.split("\n")) {
    const marker = /^ {0,3}(`{3,}|~{3,})/.exec(line)?.[1];
    if (marker) {
      if (!fence) fence = marker;
      else if (marker[0] === fence[0] && marker.length >= fence.length) fence = undefined;
    } else if (!fence) lines.push(line);
  }
  const label = /^\s*(?:#{1,6}\s+)?(?:Spoken summary|Summary|TL;DR|TLDR)(?::[ \t]*(.*)|[ \t]*)$/i;
  const section = lines.findLastIndex(line => label.test(line.replace(/\*\*/g, "")));
  const selected = section < 0 ? lines : [
    label.exec(lines[section].replace(/\*\*/g, ""))![1] ?? "",
    ...lines.slice(section + 1),
  ];
  const prose: string[] = [];
  for (let line of selected) {
    if (/^\s*#{1,6}\s/.test(line)) {
      if (prose.length) break;
      continue;
    }
    if (!line.trim()) {
      if (prose.length) break;
      continue;
    }
    if (/^\s*(?:\||[-=*]{3,}\s*$)/.test(line)) continue;
    line = line.replace(/^\s*(?:[-*+]\s+|\d+[.)]\s+|>\s*)/, "");
    prose.push(line);
  }
  const raw = prose.join(" ");
  // Check before Markdown cleanup, which would erase underscores in key prefixes.
  if (/\b(?:sk_[\w-]{16,}|sk-[\w-]{16,}|gh[pousr]_[\w]{16,}|AKIA[A-Z0-9]{16})\b|-----BEGIN/.test(raw)) return "";
  const clean = raw
    .replace(/!\[[^\]]*\]\([^)]*\)/g, "")
    .replace(/\[([^\]]+)\]\([^)]*\)/g, "$1")
    .replace(/`+[^`]*`+/g, "")
    .replace(/https?:\/\/\S+|(?:~\/|\/)[\w./-]+/g, "")
    .replace(/\[[^\]\n]+\]/g, "")
    .replace(/[*_]/g, "")
    .replace(/\s+/g, " ").trim();
  if (!/[\p{L}\p{N}]/u.test(clean)) return "";
  const summary = clean.split(/(?<=[.!?])\s+/u).slice(0, 2).join(" ");
  if (summary.length <= 500) return summary;
  const head = summary.slice(0, 499);
  return `${head.slice(0, head.lastIndexOf(" ") > 0 ? head.lastIndexOf(" ") : head.length)}…`;
}

type VoiceState = {
  run?: symbol;
  candidate?: string;
  attempted?: string;
  controller?: AbortController;
  error?: string;
};

export function createSummarySpeaker(ctx, speak = speakText) {
  // Deliberately not persisted: every new session/server starts silent.
  const sessions = new Map<string, VoiceState>();
  let closed = false;
  const cancel = (state: VoiceState) => {
    state.controller?.abort();
    state.controller = undefined;
    state.run = undefined;
    state.candidate = undefined;
  };
  const status = (id: string) => ({ enabled: sessions.has(id), error: sessions.get(id)?.error ?? "" });

  return {
    async control(sessionID: string, action: string) {
      if (closed) throw new Error("Voice integration is stopped");
      if (!["on", "off", "status"].includes(action)) throw new Error("Use /voice on, /voice off or /voice status");
      const session = await ctx.session.get({ sessionID });
      if (session.parentID) throw new Error("Automatic speech is available only in the main session");
      if (action === "on" && !sessions.has(sessionID)) sessions.set(sessionID, {});
      if (action === "off") {
        const state = sessions.get(sessionID);
        if (state) cancel(state);
        sessions.delete(sessionID);
      }
      return status(sessionID);
    },
    async handle(event) {
      const id = event.data.sessionID;
      const state = sessions.get(id);
      if (closed || !state) return;
      if (event.type === "session.deleted") {
        cancel(state);
        sessions.delete(id);
        return;
      }
      if (event.type === "session.execution.started") {
        cancel(state);
        state.run = Symbol();
        return;
      }
      if (event.type === "session.execution.failed" || event.type === "session.execution.interrupted") {
        cancel(state);
        return;
      }
      if (!state.run) return;
      if (event.type === "session.step.started") { state.candidate = undefined; return; }
      if (event.type === "session.step.ended") {
        state.candidate = event.data.finish === "stop" ? event.data.assistantMessageID : undefined;
        return;
      }
      if (event.type !== "session.execution.succeeded") return;
      const messageID = state.candidate;
      state.candidate = undefined;
      if (!messageID || state.attempted === messageID) return;
      state.attempted = messageID; // No automatic retries that could bill twice.
      const run = state.run;
      const current = () => !closed && sessions.get(id) === state && state.run === run;
      try {
        const session = await ctx.session.get({ sessionID: id });
        if (!current() || session.parentID || session.outcome !== "succeeded") return;
        const messages = await ctx.session.context({ sessionID: id });
        const reply = messages.find(message => message.id === messageID);
        if (!current() || reply?.type !== "assistant" || reply.finish !== "stop" || reply.error || !reply.time?.completed) return;
        const text = reply.content.filter(part => part.type === "text" && part.native?.phase !== "commentary")
          .map(part => part.text).join("\n");
        const summary = spokenSummary(text);
        if (!summary) return;
        await ctx.permission.assert({ action: "speak", resources: ["*"], sessionID: id, agent: reply.agent });
        if (!current()) return;
        state.controller = new AbortController();
        state.error = undefined;
        await speak({ text: PENSIVE + summary, voice_id: "JBFqnCBsd6RMkjVDRZzb", model_id: "eleven_v3" },
          { signal: state.controller.signal });
      } catch {
        if (current() && !state.controller?.signal.aborted) {
          // Only expose a known local status, not provider bodies or conversation text.
          state.error = "Speech failed. Check the ElevenLabs key/quota and audio output; use manual speak for details.";
        }
      }
    },
    stop() {
      closed = true;
      for (const state of sessions.values()) cancel(state);
      sessions.clear();
    },
  };
}

export function setupAutoSpeak(ctx) {
  const speaker = createSummarySpeaker(ctx);
  const controller = new AbortController();
  const events = (async () => {
    for await (const event of ctx.event.subscribe({
      types: ["session.execution.started", "session.step.started", "session.step.ended", "session.execution.succeeded",
        "session.execution.failed", "session.execution.interrupted", "session.deleted"],
      signal: controller.signal,
    })) {
      // Synthesis must not block newer events, especially /voice off and interruptions.
      void speaker.handle(event);
    }
  })();
  events.catch(() => { if (!controller.signal.aborted) speaker.stop(); });
  return { control: speaker.control, stop() { controller.abort(); speaker.stop(); } };
}

export function setupVoiceCommand(context) {
  context.keymap.layer(() => ({ commands: [{
    id: "session.voice",
    title: "Spoken summaries",
    description: "Automatic pensive George summaries for this session [on|off|status]; starts off",
    group: "Session",
    palette: true,
    slash: { name: "voice", arguments: true },
    async run(input?: string) {
      const route = context.ui.router.current();
      if (route.type !== "session") {
        context.ui.toast.show({ message: "Open a main session first", variant: "warning" });
        return;
      }
      const action = input?.trim().toLowerCase() || "status";
      if (!["on", "off", "status"].includes(action)) {
        context.ui.toast.show({ message: "Use /voice on, /voice off or /voice status", variant: "warning" });
        return;
      }
      try {
        const { output } = await context.client.rpc.call({ rpcID: VOICE_RPC_ID, method: "control",
          input: { sessionID: route.sessionID, action } });
        const state = output as { enabled: boolean; error: string };
        context.ui.toast.show({ title: "Spoken summaries",
          message: state.enabled ? `On for this session: pensive George. Summary text is sent to ElevenLabs.${state.error ? ` ${state.error}` : ""}` : "Off for this session",
          variant: state.error ? "warning" : "info", duration: 6000 });
      } catch {
        context.ui.toast.show({ message: "Could not change voice mode. Open a main session and check the server plugin.", variant: "error" });
      }
    },
  }] }));
}
