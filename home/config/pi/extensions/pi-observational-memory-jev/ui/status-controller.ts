import { formatFooterSpend, type Spend } from "../spend.js";

export type WorkerType = "observer" | "consolidator";

export interface FooterGauges {
	nextValue: number;
	nextMax: number;
	poolValue: number;
	poolMax: number;
	ctxValue: number;
	ctxMax: number;
}

interface Theme {
	fg(color: string, text: string): string;
}

export interface StatusUI {
	setStatus(key: string, text: string | undefined): void;
	setWidget(key: string, content: string[] | undefined): void;
	theme: Theme;
}

type WorkerState = { kind: "running" } | { kind: "done"; delta?: number } | { kind: "error" };

const FOOTER_KEY = "om";
const WORKERS_WIDGET_KEY = "om-workers";
const SPINNER_FRAMES = ["◐", "◓", "◑", "◒"] as const;
const WORKER_SEP = "   ";

export interface StatusControllerOptions {
	spinnerIntervalMs?: number;
	settleMs?: number;
}

interface WorkerEntry {
	type: WorkerType;
	state: WorkerState;
	settleTimer?: ReturnType<typeof setTimeout>;
}

export class StatusController {
	private ui: StatusUI | undefined;
	private frame = 0;
	private readonly workers = new Map<string, WorkerEntry>();
	private spinnerTimer: ReturnType<typeof setInterval> | undefined;
	private gauges: FooterGauges | undefined;
	private spend: Spend | undefined;
	private readonly spinnerIntervalMs: number;
	private readonly settleMs: number;

	constructor(options: StatusControllerOptions = {}) {
		this.spinnerIntervalMs = options.spinnerIntervalMs ?? 120;
		this.settleMs = options.settleMs ?? 5000;
	}

	attach(ui: StatusUI): void {
		this.ui = ui;
		this.ui.setStatus(FOOTER_KEY, this.renderFooter());
	}

	detach(): void {
		this.stopSpinner();
		for (const entry of this.workers.values()) {
			if (entry.settleTimer) clearTimeout(entry.settleTimer);
		}
		this.workers.clear();
		this.gauges = undefined;
		this.spend = undefined;
		this.ui?.setWidget(WORKERS_WIDGET_KEY, undefined);
		if (this.ui) this.ui.setStatus(FOOTER_KEY, undefined);
		this.ui = undefined;
	}

	setGauges(gauges: FooterGauges | undefined): void {
		this.gauges = gauges;
		if (this.ui) this.ui.setStatus(FOOTER_KEY, this.renderFooter());
	}

	setSpend(spend: Spend): void {
		this.spend = spend;
		if (this.ui) this.ui.setStatus(FOOTER_KEY, this.renderFooter());
	}

	workerStart(type: WorkerType, runId: string): void {
		if (!this.ui) return;
		const existing = this.workers.get(runId);
		if (existing?.settleTimer) clearTimeout(existing.settleTimer);
		this.workers.set(runId, { type, state: { kind: "running" } });
		this.startSpinner();
		this.renderWorkersWidget();
	}

	workerDone(runId: string, delta?: number): void {
		this.settle(runId, { kind: "done", delta });
	}

	workerError(runId: string): void {
		this.settle(runId, { kind: "error" });
	}

	private settle(runId: string, state: WorkerState): void {
		if (!this.ui) return;
		const entry = this.workers.get(runId);
		if (!entry) return;
		if (entry.settleTimer) clearTimeout(entry.settleTimer);
		entry.state = state;
		this.renderWorkersWidget();
		entry.settleTimer = setTimeout(() => {
			this.workers.delete(runId);
			this.renderWorkersWidget();
			if (!this.hasRunningWorker()) this.stopSpinner();
		}, this.settleMs);
		entry.settleTimer.unref?.();
		if (!this.hasRunningWorker()) this.stopSpinner();
	}

	private hasRunningWorker(): boolean {
		for (const entry of this.workers.values()) {
			if (entry.state.kind === "running") return true;
		}
		return false;
	}

	private startSpinner(): void {
		if (this.spinnerTimer) return;
		this.spinnerTimer = setInterval(() => {
			this.frame = (this.frame + 1) % SPINNER_FRAMES.length;
			if (this.hasRunningWorker()) this.renderWorkersWidget();
		}, this.spinnerIntervalMs);
		this.spinnerTimer.unref?.();
	}

	private stopSpinner(): void {
		if (!this.spinnerTimer) return;
		clearInterval(this.spinnerTimer);
		this.spinnerTimer = undefined;
	}

	private gaugeBar(value: number, max: number, cells = 8): string {
		const theme = this.ui!.theme;
		const frac = max <= 0 ? 0 : Math.max(0, value / max);
		const filled = Math.min(cells, Math.round(Math.min(1, frac) * cells));
		const fillColor = frac >= 1 ? "warning" : "dim";
		return (
			theme.fg(fillColor, "▕") +
			theme.fg(fillColor, "█".repeat(filled)) +
			theme.fg(fillColor, "░".repeat(cells - filled)) +
			theme.fg(fillColor, "▏")
		);
	}

	renderFooter(): string {
		const theme = this.ui?.theme;
		if (!theme) return "om";
		const g = this.gauges;
		if (!g) return `${theme.fg("success", "om")}`;
		const next = `${theme.fg("muted", "O")}${this.gaugeBar(g.nextValue, g.nextMax)}`;
		const pool = `${theme.fg("muted", "C")}${this.gaugeBar(g.poolValue, g.poolMax)}`;
		const ctx = `${theme.fg("muted", "X")}${this.gaugeBar(g.ctxValue, g.ctxMax)}`;
		const spendText = this.spend ? formatFooterSpend(this.spend) : "";
		const cost = spendText ? ` ${theme.fg("dim", spendText)}` : "";
		return `${next}  ${pool}  ${ctx}${cost}`;
	}

	private renderWorkersWidget(): void {
		const ui = this.ui;
		if (!ui) return;
		if (this.workers.size === 0) {
			ui.setWidget(WORKERS_WIDGET_KEY, undefined);
			return;
		}
		const theme = ui.theme;
		const parts: string[] = [];
		for (const entry of this.workers.values()) {
			if (entry.state.kind === "running") {
				parts.push(`${theme.fg("accent", SPINNER_FRAMES[this.frame])} ${theme.fg("accent", `[${entry.type}]`)}`);
			} else if (entry.state.kind === "error") {
				parts.push(`${theme.fg("error", "✗")} ${theme.fg("muted", `[${entry.type}]`)}`);
			} else {
				const delta =
					entry.state.delta && entry.state.delta > 0 ? ` ${theme.fg("success", `+${entry.state.delta}`)}` : "";
				parts.push(`${theme.fg("success", "✓")} ${theme.fg("muted", `[${entry.type}]`)}${delta}`);
			}
		}
		ui.setWidget(WORKERS_WIDGET_KEY, [parts.join(WORKER_SEP)]);
	}
}
