import test from "node:test";
import assert from "node:assert/strict";
import { applyWorkLabel, LABEL_MAX, teamWorkLabel } from "../home/config/pi/extensions/subagent/labels.ts";

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
	assert.equal(applyWorkLabel({ setSessionName: (name) => names.push(name) }, "sceptic", "Task: review"), "sceptic: review");
	assert.deepEqual(names, ["sceptic: review"]);
	assert.equal(applyWorkLabel({}, "builder"), "builder");
	assert.equal(
		applyWorkLabel({
			setSessionName: () => {
				throw new Error("session name unavailable");
			},
		}, "builder"),
		"builder",
	);
});
