import { OM_ENABLED, type Entry } from "./ledger/index.js";

export function readEnabledFromLedger(branch: Entry[]): boolean | undefined {
	for (let i = branch.length - 1; i >= 0; i--) {
		const entry = branch[i];
		if (entry.type === "custom" && entry.customType === OM_ENABLED) {
			return (entry.data as { enabled?: boolean } | undefined)?.enabled ?? false;
		}
	}
	return undefined;
}

export function initialOmEnabled(branch: Entry[], enabledByDefault: boolean): boolean {
	return readEnabledFromLedger(branch) ?? enabledByDefault;
}
