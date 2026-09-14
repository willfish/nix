// Real Pi command, lifecycle and detached auditor transport. Local fake model only.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { mkdtemp, mkdir, writeFile, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';
import test from 'node:test';

const binary = process.env.PI_GOAL_TEST_BIN ?? process.env.PI_SESSION_TEST_BIN;
test('real Pi /goal audits with restricted tools, bounds loops, pauses and restores', { skip: !binary, timeout: 60000 }, async () => {
  const dir = await mkdtemp(join(tmpdir(), 'pi-goal-runtime-'));
  const histories = [], wire = [], pending = new Map(), heldMain = [];
  let child, buffer = '', stderr = '', sequence = 0, mode = 'complete';
  const server = createServer(async (req, res) => {
    try {
      let body = ''; for await (const chunk of req) body += chunk;
      const payload = JSON.parse(body); histories.push(payload);
      const system = payload.messages.filter(message => ['system', 'developer'].includes(message.role)).map(message => message.content).join('\n');
      const audit = system.includes('independent evidence auditor');
      let text, call;
      if (audit) {
        assert.deepEqual(payload.tools.map(tool => tool.function.name).sort(), ['find', 'grep', 'ls', 'read']);
        assert.doesNotMatch(system, /Active goal, revision/);
        if (!payload.messages.some(message => message.role === 'tool')) {
          call = { index: 0, id: 'inspect-1', type: 'function', function: { name: 'read', arguments: JSON.stringify({ path: join(dir, 'proof.txt') }) } };
        } else {
          const prompt = payload.messages.filter(message => message.role === 'user').map(message => typeof message.content === 'string' ? message.content : message.content.map(part => part.text ?? '').join('\n')).join('\n');
          const raw = prompt.match(/<untrusted_contract>\n([\s\S]*?)\n<\/untrusted_contract>/)[1];
          const contract = JSON.parse(raw.replaceAll('&lt;', '<').replaceAll('&gt;', '>').replaceAll('&amp;', '&'));
          text = JSON.stringify({ auditId: contract.auditId, goalId: contract.id, revision: contract.revision, verdict: 'PASS',
            requirements: contract.requirements.map(item => ({ id: item.id, status: 'verified', evidence: ['proof.txt:1 contains fixture proof'] })) });
        }
      } else text = ['complete', 'hold-audit'].includes(mode) ? 'Completion claim\n<!--goal:complete-->' : mode === 'waiting' ? 'Please approve\n<!--goal:waiting-->' : 'Still working';
      res.writeHead(200, { 'content-type': 'text/event-stream' });
      if ((audit && mode === 'hold-audit') || (!audit && mode === 'hold-main')) {
        res.flushHeaders();
        return; // Client cancellation must close this deliberately stalled stream.
      }
      const chunk = (delta, finish_reason = null) => res.write(`data: ${JSON.stringify({ id: 'fixture', object: 'chat.completion.chunk', created: 1, model: 'echo', choices: [{ index: 0, delta, finish_reason }] })}\n\n`);
      const finish = () => {
        chunk({ role: 'assistant' }); chunk(call ? { tool_calls: [call] } : { content: text });
        chunk({}, call ? 'tool_calls' : 'stop'); res.end('data: [DONE]\n\n');
      };
      if (!audit && mode === 'defer-main') { heldMain.push(finish); res.flushHeaders(); }
      else finish();
    } catch (error) {
      stderr += `fixture: ${error.stack}\n`;
      res.writeHead(500).end('Fixture error');
    }
  });
  try {
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    const agentDir = join(dir, 'agent'); await mkdir(agentDir);
    await writeFile(join(dir, 'proof.txt'), 'fixture proof\n');
    await writeFile(join(agentDir, 'models.json'), JSON.stringify({ providers: { 'goal-smoke': {
      baseUrl: `http://127.0.0.1:${server.address().port}/v1`, api: 'openai-completions', apiKey: 'local-test-placeholder',
      compat: { supportsDeveloperRole: false, supportsUsageInStreaming: false },
      models: [{ id: 'echo', reasoning: false, contextWindow: 128000, maxTokens: 2048 }],
    } } }));
    await writeFile(join(agentDir, 'settings.json'), JSON.stringify({ defaultProvider: 'goal-smoke', defaultModel: 'echo', enableInstallTelemetry: false, retry: { enabled: false }, compaction: { keepRecentTokens: 64, reserveTokens: 2048 } }));
    const extension = process.env.PI_GOAL_TEST_EXTENSION ?? (process.env.PI_HARNESS_TEST_HOME_FILES
      ? join(process.env.PI_HARNESS_TEST_HOME_FILES, '.pi/agent/extensions/goal.ts')
      : fileURLToPath(new URL('../home/config/pi/extensions/goal.ts', import.meta.url)));
    child = spawn(binary, ['--mode', 'rpc', '--offline', '--no-extensions', '--no-skills', '--no-prompt-templates', '--no-context-files', '-e', extension],
      { cwd: dir, env: { ...process.env, PATH: `${dirname(binary)}:${process.env.PATH}`, PI_CODING_AGENT_DIR: agentDir, PI_OFFLINE: '1', CAPTURE_PROMPTS: '0' }, stdio: ['pipe', 'pipe', 'pipe'] });
    child.on('error', error => { stderr += `Pi startup failed: ${error.code}\n`; });
    child.stderr.setEncoding('utf8').on('data', text => { stderr += text; });
    child.stdout.setEncoding('utf8').on('data', text => {
      buffer += text;
      let newline;
      while ((newline = buffer.indexOf('\n')) >= 0) {
        const line = buffer.slice(0, newline); buffer = buffer.slice(newline + 1);
        if (!line.trim()) continue;
        const event = JSON.parse(line); wire.push(event);
        if (event.type === 'response') pending.get(event.id)?.(event);
        if (event.type === 'extension_ui_request' && event.method === 'confirm') {
          child.stdin.write(JSON.stringify({ type: 'extension_ui_response', id: event.id, confirmed: true }) + '\n');
        }
      }
    });
    function request(type, args = {}) {
      return new Promise((resolve, reject) => {
        const id = String(++sequence);
        const timer = setTimeout(() => { pending.delete(id); reject(Error(`Timeout ${type}: ${stderr}`)); }, 15000);
        pending.set(id, event => { clearTimeout(timer); pending.delete(id); event.success ? resolve(event.data) : reject(Error(JSON.stringify(event))); });
        child.stdin.write(JSON.stringify({ id, type, ...args }) + '\n');
      });
    }
    async function until(predicate, description) {
      for (let i = 0; i < 300; i++) {
        if (predicate()) return;
        if (child.exitCode !== null) throw Error(`Pi exited: ${stderr}`);
        await delay(25);
      }
      throw Error(`Timeout ${description}: ${stderr}; recent events ${JSON.stringify(wire.slice(-8))}`);
    }
    const status = () => wire.filter(event => event.type === 'extension_ui_request' && event.method === 'setStatus' && event.statusKey === 'goal').at(-1)?.statusText ?? '';
    const command = message => request('prompt', { message });
    assert.ok((await request('get_commands')).commands.some(command => command.name === 'goal'));
    await command('/goal proof.txt contains fixture proof');
    await until(() => status().includes('complete'), 'independent audit completion');
    assert.equal(histories.length, 3, 'one implementation turn and two detached auditor turns');
    const sessionFile = (await request('get_state')).sessionFile;
    const journal = (await readFile(sessionFile, 'utf8')).split('\n').filter(Boolean).map(line => JSON.parse(line));
    const anchors = journal.filter(entry => entry.customType === 'goal-audit-card');
    const results = journal.filter(entry => entry.customType === 'goal-audit-result');
    assert.equal(anchors.length, 1); assert.equal(results.length, 1);
    assert.equal(results[0].data.phase, 'PASS');
    assert.equal(results[0].data.tools[0].tool, 'read');
    assert.equal(results[0].data.tools[0].state, 'done');
    assert.equal(results[0].data.tools[0].path, join(dir, 'proof.txt'));
    assert.doesNotMatch(JSON.stringify(histories), /goal-audit-card|Inspection finished|Starting independent auditor/);
    const stateEntries = journal.filter(entry => entry.customType === 'goal');
    assert.equal(stateEntries.at(-1).data.goal.status, 'complete');
    assert.equal(stateEntries.at(-1).data.goal.audits, 1);
    await command('/goal resume'); assert.equal(histories.length, 3);
    mode = 'hold-audit'; await command('/goal inspect proof again');
    await until(() => status().includes('auditing'), 'audit start before cancellation');
    await command('/goal pause');
    await until(() => status().includes('paused'), 'audit cancellation');
    assert.ok(!(await request('get_state')).isStreaming);
    mode = 'hold-main'; const beforeAbort = histories.length; await command('/goal resume');
    await until(() => histories.length > beforeAbort && !JSON.stringify(histories.at(-1).messages).includes('independent evidence auditor'), 'main run before abort');
    await request('abort');
    await until(() => status().includes('paused'), 'Escape-equivalent abort');
    mode = 'waiting';
    await request('compact');
    assert.match(status(), /paused/, 'compaction retains the paused goal');
    mode = 'waiting'; await command('/goal requires a human decision');
    await until(() => status().includes('paused'), 'approval pause');
    const beforeResume = histories.length;
    mode = 'defer-main';
    const approval = command('Approved, proceed with the work');
    await until(() => heldMain.length === 1, 'approval-triggered work');
    await command('/goal resume'); await command('/goal resume');
    assert.match(status(), /paused/, 'resume must return promptly without activating during work');
    await request('follow_up', { message: 'Also inspect the queued clarification' });
    heldMain.shift()(); await approval;
    await until(() => heldMain.length === 1, 'queued follow-up work');
    assert.equal(histories.length, beforeResume + 2);
    assert.match(status(), /paused/, 'low-level agent end must not consume resume before follow-ups');
    mode = 'waiting'; heldMain.shift()();
    await until(() => histories.length === beforeResume + 3 && status().includes('paused'), 'one deferred continuation after full settlement');
    const resumedJournal = (await readFile(sessionFile, 'utf8')).split('\n').filter(Boolean).map(line => JSON.parse(line));
    const resume = resumedJournal.filter(entry => entry.customType === 'goal' && entry.data.action === 'resume').at(-1);
    assert.equal(resume.data.goal.continuations, 0); assert.equal(resume.data.goal.audits, 0);
    const sinceResume = resumedJournal.slice(resumedJournal.indexOf(resume) + 1);
    assert.equal(sinceResume.filter(entry => entry.customType === 'goal' && entry.data.action === 'continue').length, 1);
    const count = histories.length;
    await request('get_state'); assert.equal(histories.length, count);
    mode = 'loop'; await command('/goal resume');
    await until(() => status().includes('limited'), 'bounded automatic continuation');
    assert.equal(histories.length - count, 11, 'initial resumed turn plus ten automatic continuations');
    await command('/goal pause');
    await command('/goal edit changed acceptance criteria');
    await request('new_session');
    await request('switch_session', { sessionPath: sessionFile });
    await command('/goal status'); assert.match(status(), /paused/);
    assert.equal(histories.length - count, 11, 'session restore does not authorize more work');
    assert.equal(wire.filter(event => event.type === 'extension_error').length, 0, stderr);
    assert.equal(stderr.includes('fixture:'), false, stderr);
  } finally {
    if (child && child.exitCode === null) { child.kill('SIGTERM'); await new Promise(resolve => child.once('exit', resolve)); }
    await new Promise(resolve => { server.close(resolve); server.closeAllConnections(); });
    await rm(dir, { recursive: true, force: true });
  }
});
