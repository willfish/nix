import { createServer } from 'node:http';

// Loopback OpenAI SSE only. Never stores headers or makes upstream requests.
export async function questionModel() {
  const histories = [], toolcalls = [], errors = [];
  let releaseSlow;
  const slow = new Promise(resolve => { releaseSlow = resolve; });
  const server = createServer(async (request, response) => {
    if (request.method !== 'POST' || request.url !== '/v1/chat/completions') {
      response.writeHead(404).end();
      return;
    }
    try {
      let body = '';
      for await (const chunk of request) {
        body += chunk;
        if (body.length > 2 * 1024 * 1024) throw new Error('Oversized request');
      }
      const payload = JSON.parse(body);
      if (payload.model !== 'question' || payload.stream !== true || !Array.isArray(payload.messages)) {
        throw new Error('Expected streaming question model');
      }
      histories.push(structuredClone(payload));
      const users = payload.messages.filter(message => message.role === 'user');
      const latest = JSON.stringify(users.at(-1)?.content);
      const marker = latest.match(/Q_[A-Z_]+/)?.[0] ?? 'Q_NONE';
      const available = payload.tools?.some(tool => tool.function.name === 'ask_coordinator');
      const asking = available && ['Q_PAR', 'Q_CHAIN', 'Q_REPEAT', 'Q_AGAIN', 'Q_HUMAN', 'Q_CANCEL'].includes(marker);
      // A genuine terminate result must prevent this extra provider request.
      const unanswered = payload.messages.at(-1)?.role === 'tool';
      if (marker === 'Q_SLOW') await slow;
      response.writeHead(200, { 'content-type': 'text/event-stream', 'cache-control': 'no-cache' });
      const chunk = (delta, finish_reason = null) => response.write(`data: ${JSON.stringify({
        id: `question-${histories.length}`, object: 'chat.completion.chunk', created: 1, model: 'question',
        choices: [{ index: 0, delta, finish_reason }],
      })}\n\n`);
      chunk({ role: 'assistant', content: '' });
      if (asking && !unanswered) {
        const args = { text: `Clarify ${marker}?`, requiresUser: marker === 'Q_HUMAN',
          ...(marker === 'Q_HUMAN' ? { choices: ['Approve', 'Decline'] } : {}) };
        const call = { id: `call-${toolcalls.length + 1}`, type: 'function',
          function: { name: 'ask_coordinator', arguments: JSON.stringify(args) } };
        toolcalls.push({ marker, at: Date.now(), call: structuredClone(call) });
        chunk({ tool_calls: [{ index: 0, ...call }] });
        chunk({}, 'tool_calls');
      } else {
        chunk({ content: unanswered ? 'UNEXPECTED_EXTRA_TURN' : `final:${marker}; user-turns:${users.length}` });
        chunk({}, 'stop');
      }
      response.end('data: [DONE]\n\n');
    } catch (error) {
      errors.push(String(error));
      response.writeHead(400).end('Invalid question request');
    }
  });
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  return {
    histories, toolcalls, errors, releaseSlow,
    models: { providers: { 'team-question': {
      baseUrl: `http://127.0.0.1:${server.address().port}/v1`, api: 'openai-completions',
      apiKey: 'local-test-placeholder',
      compat: { supportsDeveloperRole: false, supportsUsageInStreaming: false },
      models: [{ id: 'question', reasoning: false, contextWindow: 128000, maxTokens: 256 }],
    } } },
    close: () => { releaseSlow(); return new Promise((resolve, reject) => {
      server.close(error => error ? reject(error) : resolve());
      server.closeAllConnections();
    }); },
  };
}
