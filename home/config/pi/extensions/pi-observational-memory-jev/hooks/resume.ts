/** Team children own turns through the bridge. Do not inject a hidden resume. */
export function shouldResumeAfterOmCompact(
	runtime: { enabled: boolean; config: { resumeAfterMidRunCompaction: boolean; passive: boolean } },
	willContinue: boolean,
	env: NodeJS.ProcessEnv = process.env,
): boolean {
	return Boolean(
		willContinue &&
			runtime.config.resumeAfterMidRunCompaction &&
			runtime.enabled &&
			!runtime.config.passive &&
			env.PI_TEAM_CHILD !== "1",
	);
}
