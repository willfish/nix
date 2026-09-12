import { createServer } from 'node:http';

// No model implementation or external requests: only the OpenAI streaming wire format.
export async function smokeModel() {
  const histories = [];
  const server = createServer(async (request, response) => {
    if (request.method !== 'POST' || request.url !== '/v1/chat/completions') {
      response.writeHead(404).end();
      return;
    }
    try {
      let body = '';
      for await (const chunk of request) {
        body += chunk;
        if (body.length > 2 * 1024 * 1024) throw new Error('Oversized smoke request');
      }
      const payload = JSON.parse(body);
      if (payload.model !== 'echo' || payload.stream !== true || !Array.isArray(payload.messages)) {
        throw new Error('Expected streaming echo model request');
      }
      histories.push(structuredClone(payload)); // Deliberately never retain HTTP/auth headers.
      const users = payload.messages.filter(message => message.role === 'user');
      const latest = JSON.stringify(users.at(-1)?.content);
      const marker = latest.match(/SMOKE_[A-Z0-9_]+/)?.[0] ?? 'SMOKE_NO_MARKER';
      const text = `echo:${marker}; user-turns:${users.length}`;
      response.writeHead(200, { 'content-type': 'text/event-stream', 'cache-control': 'no-cache' });
      const chunk = (delta, finish_reason = null) => response.write(`data: ${JSON.stringify({
        id: `smoke-${histories.length}`, object: 'chat.completion.chunk', created: 1, model: 'echo',
        choices: [{ index: 0, delta, finish_reason }],
      })}\n\n`);
      chunk({ role: 'assistant', content: '' });
      chunk({ content: text.slice(0, 5) });
      chunk({ content: text.slice(5) });
      chunk({}, 'stop');
      response.end('data: [DONE]\n\n');
    } catch {
      response.writeHead(400).end('Invalid smoke request');
    }
  });
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  return {
    histories,
    models: { providers: { 'team-smoke': {
      baseUrl: `http://127.0.0.1:${server.address().port}/v1`, api: 'openai-completions',
      apiKey: 'local-test-placeholder',
      compat: { supportsDeveloperRole: false, supportsUsageInStreaming: false },
      models: [{ id: 'echo', reasoning: false, contextWindow: 128000, maxTokens: 256 }],
    } } },
    close: () => new Promise((resolve, reject) => {
      server.close(error => error ? reject(error) : resolve());
      server.closeAllConnections();
    }),
  };
}
