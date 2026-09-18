import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";
import { OM_RESUME, rawTokensSinceLastCompaction, type Entry } from "../ledger/index.js";
import type { Runtime } from "../runtime.js";

const RESUME_PROMPT =
	"[automatic] Your context was just compacted to free space; no user message was sent. " +
	"Continue exactly where you left off, as if the compaction had not happened.";

const RETRYABLE_ERROR_RE =
	/overloaded|provider.?returned.?error|rate.?limit|too many requests|429|500|502|503|504|service.?unavailable|server.?error|internal.?error|network.?error|connection.?error|connection.?refused|connection.?lost|websocket.?closed|websocket.?error|other side closed|fetch failed|upstream.?connect|reset before headers|socket hang up|ended without|http2 request did not get a response|timed? out|timeout|terminated|retry delay/i;

function contextPressureTokens(
	ctx: { getContextUsage?: () => { tokens: number | null } | undefined; sessionManager: { getBranch: () => Entry[] } },
	threshold: number,
): { tokens: number; due: boolean } {
	const live = ctx.getContextUsage?.()?.tokens;
	if (live != null) return { tokens: live, due: live >= threshold };
	const raw = rawTokensSinceLastCompaction(ctx.sessionManager.getBranch());
	return { tokens: raw, due: raw >= threshold };
}

function turnWillContinue(event: any): boolean {
	const toolResults = event?.toolResults;
	if (Array.isArray(toolResults) && toolResults.length > 0) return true;
	const stop = event?.message?.stopReason;
	return stop === "tool_use" || stop === "tool_calls";
}

export function registerCompactionTrigger(pi: ExtensionAPI, runtime: Runtime): void {
	pi.on("turn_end", (event: any, ctx: any) => {
		if (!runtime.enabled || runtime.config.passive) return;
		if (runtime.compactInFlight) return;

		const message = event?.message;
		if (
			message?.role === "assistant" &&
			message.stopReason === "error" &&
			message.errorMessage &&
			RETRYABLE_ERROR_RE.test(message.errorMessage)
		) {
			return;
		}

		if (!contextPressureTokens(ctx, runtime.config.compactAtContextTokens).due) return;

		const shouldResume = runtime.config.resumeAfterMidRunCompaction && turnWillContinue(event);
		const hasUI = ctx.hasUI;
		const ui = ctx.ui;
		runtime.compactInFlight = true;
		if (hasUI) ui?.notify("om: context threshold reached — compacting (waiting for in-flight observers)…", "info");

		ctx.compact({
			onComplete: () => {
				runtime.compactInFlight = false;
				if (hasUI) ui?.notify("om: compaction complete", "info");
				if (!shouldResume || !runtime.enabled || runtime.config.passive) return;
				try {
					pi.sendMessage(
						{ customType: OM_RESUME, content: RESUME_PROMPT, display: false },
						{ triggerTurn: true },
					);
				} catch (error) {
					const msg = error instanceof Error ? error.message : String(error);
					runtime.lastWorkerError = `resume failed: ${msg}`;
					if (hasUI) ui?.notify(`om: resume failed — ${msg}`, "error");
				}
			},
			onError: (error: { message: string }) => {
				runtime.compactInFlight = false;
				if (error.message === "Compaction cancelled") return;
				if (hasUI) ui?.notify(`om: ${error.message}`, "error");
			},
		});
	});
}
