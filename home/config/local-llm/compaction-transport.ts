import type { Request } from './compaction';

/** All requests stay on the authenticated llama-server origin. */
export function createTransport(
  fetcher: typeof fetch,
  headers: () => HeadersInit,
  signal?: AbortSignal,
) {
  async function call(url: string, body?: unknown): Promise<Response> {
    signal?.throwIfAborted();
    const response = await fetcher(url, {
      method: body === undefined ? 'GET' : 'POST',
      headers: headers(), body: body === undefined ? undefined : JSON.stringify(body), signal,
    });
    signal?.throwIfAborted();
    if (!response.ok) {
      const error = await response.json().catch(() => ({}));
      throw Error(`Context compaction: ${error.error?.message || `server returned HTTP ${response.status}`}`);
    }
    return response;
  }
  return {
    async metadata(model?: string) {
      const props = await (await call(`./props${model ? `?model=${encodeURIComponent(model)}` : ''}`)).json();
      return {
        contextSize: props.default_generation_settings?.n_ctx as number,
        identity: JSON.stringify({ model: props.model_path, template: props.chat_template }),
      };
    },
    async count(body: Request): Promise<number> {
      if (body.messages.some(m => Array.isArray(m.content) && m.content.some(p => p.type !== 'text'))) {
        throw Error('Automatic compaction is currently text-only; image/audio token accounting is not supported.');
      }
      const rendered = await (await call('./apply-template', { ...body, stream: false })).json();
      if (typeof rendered.prompt !== 'string') throw Error('Context compaction: missing rendered prompt.');
      const tokenized = await (await call('./tokenize', {
        content: rendered.prompt, model: body.model, add_special: true, parse_special: true,
      })).json();
      if (!Array.isArray(tokenized.tokens)) throw Error('Context compaction: missing tokenizer result.');
      return tokenized.tokens.length;
    },
    async summarise(body: Request): Promise<string> {
      const response = await call('./v1/chat/completions', { ...body, stream: true });
      if (!response.body) throw Error('Context compaction: missing summary stream.');
      const reader = response.body.getReader();
      const decoder = new TextDecoder();
      let buffer = '';
      let result = '';
      let finished = false;
      let finishReason = '';
      const line = (value: string) => {
        if (!value.startsWith('data:')) return;
        const data = value.slice(5).trim();
        if (data === '[DONE]') { finished = true; return; }
        if (!data) return;
        const event = JSON.parse(data);
        if (event.error) throw Error(`Context compaction: ${event.error.message || 'summary stream error'}`);
        const choice = event.choices?.[0];
        if (choice?.delta?.tool_calls?.length) throw Error('Unexpected tool call during summarisation.');
        if (typeof choice?.delta?.content === 'string') result += choice.delta.content;
        if (choice?.finish_reason) finishReason = choice.finish_reason;
      };
      try {
        while (!finished) {
          signal?.throwIfAborted();
          const { done, value } = await reader.read();
          signal?.throwIfAborted();
          buffer += decoder.decode(value, { stream: !done });
          const lines = buffer.split('\n');
          buffer = lines.pop() || '';
          for (const entry of lines) line(entry.trimEnd());
          if (done) { if (buffer) line(buffer.trimEnd()); break; }
        }
      } finally {
        await reader.cancel().catch(() => {});
        reader.releaseLock();
      }
      signal?.throwIfAborted();
      if (finishReason === 'length') throw Error('Summary was truncated. Full history is unchanged; retry or shorten the conversation.');
      if (!finished || finishReason !== 'stop') throw Error('Incomplete summary stream. Full history is unchanged.');
      return result;
    },
  };
}
