export type Spend = {
	costUsd: number;
	runs: number;
	requests: number;
	inputTokens: number;
	outputTokens: number;
};

export const EMPTY_SPEND: Spend = {
	costUsd: 0,
	runs: 0,
	requests: 0,
	inputTokens: 0,
	outputTokens: 0,
};

export function formatTok(n: number): string {
	if (n >= 10_000) return `${Math.round(n / 1000)}k`;
	if (n >= 1000) {
		const k = n / 1000;
		const text = k >= 10 ? k.toFixed(0) : k.toFixed(1);
		return `${text.replace(/\.0$/, "")}k`;
	}
	return n.toLocaleString();
}

export function formatStatusSpend(spend: Spend): string {
	if (spend.requests <= 0 && spend.costUsd <= 0) return "none yet";
	const tok = spend.inputTokens + spend.outputTokens;
	const requestText = `${spend.requests} jev request${spend.requests === 1 ? "" : "s"}`;
	if (spend.costUsd > 0) {
		const tokPart = tok > 0 ? `, ${tok.toLocaleString()} tok` : "";
		return `$${spend.costUsd.toFixed(4)} (${requestText}${tokPart})`;
	}
	return tok > 0 ? `${requestText}, ${tok.toLocaleString()} tok` : requestText;
}

export function formatFooterSpend(spend: Spend): string {
	if (spend.costUsd > 0) return `$${spend.costUsd.toFixed(3)}`;
	if (spend.requests <= 0) return "";
	const tok = spend.inputTokens + spend.outputTokens;
	return tok > 0 ? `${spend.requests}j ${formatTok(tok)}` : `${spend.requests}j`;
}

export function formatObserverResultToast(opts: { kept: number; rejected: number; tokens: number }): string {
	const tok = `~${opts.tokens.toLocaleString()} tok`;
	if (opts.kept > 0) return `om: observer +${opts.kept} (${tok})`;
	if (opts.rejected > 0) return `om: observer covered ${tok}, kept 0`;
	return `om: observer covered ${tok}, no candidates`;
}

export function isPiCompactTooSmall(message: string): boolean {
	return /nothing to compact|too small/i.test(message);
}

export function shouldAttachOmStatus(ctx: { hasUI?: boolean; ui?: unknown }): boolean {
	return Boolean(ctx.hasUI && ctx.ui);
}
