import test from "node:test";
import assert from "node:assert/strict";
import { applyWorkLabel, LABEL_MAX, SWITCHBOARD_LABEL_ENTRY, teamWorkLabel } from "../home/config/pi/extensions/subagent/labels.ts";

test("team labels use the role, then the current task, without depending on a model tool", () => {
	assert.equal(teamWorkLabel("builder"), "builder");
	assert.equal(teamWorkLabel("builder", "Implement the importer"), "builder: Implement the importer");
	assert.equal(teamWorkLabel("builder", "Task: Implement the importer"), "builder: Implement the importer");
	assert.equal(teamWorkLabel("  scout\n", "\nTask:  map the module\u001b "), "scout: map the module");
	assert.equal(teamWorkLabel("", "only a task"), "team: only a task");
	assert.equal(teamWorkLabel(undefined), "team");
	assert.equal(teamWorkLabel("builder", "x".repeat(500)).length, LABEL_MAX);
	assert.ok(teamWorkLabel("builder", "x".repeat(500)).startsWith("builder: "));
});

test("applyWorkLabel is cosmetic and never throws", () => {
	const names: string[] = [];
	const entries: unknown[] = [];
	assert.equal(
		applyWorkLabel({
			setSessionName: (name) => names.push(name),
			appendEntry: (type, data) => entries.push([type, data]),
		}, "sceptic", "Task: review"),
		"sceptic: review",
	);
	assert.deepEqual(names, ["sceptic: review"]);
	assert.deepEqual(entries, [[SWITCHBOARD_LABEL_ENTRY, { label: "sceptic: review" }]]);
	assert.equal(applyWorkLabel({}, "builder"), "builder");
	assert.equal(
		applyWorkLabel({
			setSessionName: () => {
				throw new Error("session name unavailable");
			},
			appendEntry: () => {
				throw new Error("entry unavailable");
			},
		}, "builder"),
		"builder",
	);
});

test("startup does not clobber a launch session name, but prompts still refresh it", () => {
	const names: string[] = ["builder: Implement the importer"];
	const entries: unknown[] = [];
	const pi = {
		getSessionName: () => names.at(-1),
		setSessionName: (name: string) => names.push(name),
		appendEntry: (type: string, data?: unknown) => entries.push([type, data]),
	};
	assert.equal(applyWorkLabel(pi, "builder"), "builder: Implement the importer");
	assert.deepEqual(names, ["builder: Implement the importer"]);
	assert.deepEqual(entries, [[SWITCHBOARD_LABEL_ENTRY, { label: "builder: Implement the importer" }]]);
	assert.equal(applyWorkLabel(pi, "builder", "Task: follow up"), "builder: follow up");
	assert.deepEqual(names, ["builder: Implement the importer", "builder: follow up"]);
	assert.deepEqual(entries, [
		[SWITCHBOARD_LABEL_ENTRY, { label: "builder: Implement the importer" }],
		[SWITCHBOARD_LABEL_ENTRY, { label: "builder: follow up" }],
	]);
});
