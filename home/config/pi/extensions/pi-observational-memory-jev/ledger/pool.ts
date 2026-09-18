import { sortObservations } from "./render.js";
import type { Observation } from "./types.js";

export function poolTokens(observations: Observation[]): number {
	let total = 0;
	for (const observation of observations) total += observation.tokenCount;
	return total;
}

export type PromotionOverflow = {
	promote: Observation[];
	keptTokens: number;
	totalTokens: number;
};

export function selectPromotionOverflow(active: Observation[], poolTargetTokens: number): PromotionOverflow {
	const sorted = sortObservations(active);
	const totalTokens = poolTokens(sorted);

	let keptTokens = 0;
	let firstKeptIdx = sorted.length;
	for (let i = sorted.length - 1; i >= 0; i--) {
		const t = sorted[i].tokenCount;
		if (firstKeptIdx !== sorted.length && keptTokens + t > poolTargetTokens) break;
		keptTokens += t;
		firstKeptIdx = i;
	}

	return { promote: sorted.slice(0, firstKeptIdx), keptTokens, totalTokens };
}
