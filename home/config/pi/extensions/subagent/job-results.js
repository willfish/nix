export function segmentResult(base, segment) {
  const messages = [...(base.messages ?? []), ...(segment.messages ?? [])];
  const usage = { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, cost: 0, contextTokens: 0, turns: 0 };
  for (const message of messages) {
    if (message.role !== 'assistant') continue;
    usage.turns++;
    if (!message.usage) continue;
    for (const key of ['input', 'output', 'cacheRead', 'cacheWrite']) usage[key] += message.usage[key] || 0;
    usage.cost += message.usage.cost?.total || 0;
    usage.contextTokens = message.usage.totalTokens || 0;
  }
  const status = ['completed', 'waiting_question', 'aborted'].includes(segment.status) ? segment.status : 'error';
  return { ...base, messages, usage, status, text: segment.text ?? '',
    memberId: segment.memberId, paneId: segment.paneId, question: segment.question,
    sessionId: segment.sessionId, sessionFile: segment.sessionFile,
    exitCode: status === 'completed' || status === 'waiting_question' ? 0 : 1,
    stopReason: segment.stopReason, errorMessage: segment.errorMessage };
}

/** A question is never passed off as final task output or a successful chain step. */
export function formatJob(snapshot) {
  const lines = [`Job ${snapshot.jobId} [${snapshot.status}] revision ${snapshot.revision}`];
  for (const task of snapshot.tasks) {
    lines.push(`\n${task.index + 1}. ${task.agent}: ${task.state}`);
    if (task.cleanupError) lines.push(task.cleanupError);
    const result = task.result;
    if (!result) continue;
    if (result.memberId) lines.push(`Team member: ${result.memberId} (${task.agent})`);
    if (task.state === 'waiting_question') {
      const q = result.question;
      lines.push(`Question ${q.id}${q.requiresUser ? ' [human answer required]' : ''}: ${q.text}`);
      if (q.choices?.length) lines.push(`Choices: ${q.choices.join('; ')}`);
    } else if (['completed', 'error', 'aborted'].includes(task.state)) {
      const text = result.errorMessage || result.text || '(no output)';
      lines.push(text.length > 50000 ? `${text.slice(0, 50000)}\n[Truncated; inspect the child session.]` : text);
    }
  }
  if (snapshot.status === 'waiting_question') lines.push('\nAnswer with team answer, or use team ask to ask the human in the main pane. Then team wait for this job. Do not restart the task or chain.');
  else if (snapshot.status === 'running') {
    if (snapshot.blockedByQuestions?.length) lines.push(`\nCapacity is held by questions: ${snapshot.blockedByQuestions.join(', ')}. Use team questions and answer them before waiting again.`);
    lines.push('\nUse team wait for this job. Running and queued work remains owned by the job.');
  }
  return lines.join('\n');
}
export function jobToolResult(snapshot) {
  const value = { content: [{ type: 'text', text: formatJob(snapshot) }],
    details: { mode: snapshot.mode, job: snapshot, results: snapshot.tasks.flatMap(task => task.result ? [task.result] : []) } };
  if (['error', 'aborted'].includes(snapshot.status)) value.isError = true;
  return value;
}

/** Block every sibling before effects, not merely the delegation call itself. */
export function registerParentBatchGuard(pi, active) {
  let blocked = new Set();
  pi.on('message_end', event => {
    if (!active() || event.message?.role !== 'assistant') return;
    const calls = event.message.content?.filter(part => part.type === 'toolCall') ?? [];
    const blocking = calls.some(call => call.name === 'subagent' ||
      (call.name === 'team' && ['send', 'wait', 'ask'].includes(call.arguments?.action)));
    blocked = new Set(blocking && calls.length > 1 ? calls.map(call => call.id) : []);
  });
  pi.on('tool_call', event => {
    if (active() && blocked.has(event.toolCallId)) return {
      block: true, reason: 'Call subagent/team send/wait/ask alone. Use subagent tasks for internal parallel work so questions can return control.',
    };
  });
}
