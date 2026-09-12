import { compactRequest, type CompactionCache, type Request } from './compaction.ts';
import { createTransport } from './compaction-transport.ts';

export type ChatCompactionPorts = {
  fetch: typeof fetch;
  headers: () => HeadersInit;
  load: (conversationId: string) => Promise<CompactionCache | undefined>;
  save: (conversationId: string, cache: CompactionCache) => Promise<void>;
  toast: {
    loading: (text: string, opts: { id: string; duration: number }) => void;
    success: (text: string, opts: { id: string; duration: number }) => void;
    dismiss: (id: string) => void;
  };
};

/** Used only for conversation requests, never title generation or API clients. */
export async function compactForChatWith<T extends { messages: unknown[]; model?: string }>(
  ports: ChatCompactionPorts,
  request: T,
  conversationId: string,
  signal?: AbortSignal,
): Promise<T> {
  const transport = createTransport(ports.fetch, ports.headers, signal);
  const metadata = await transport.metadata(request.model);
  const toastId = `local-compaction-${conversationId}`;
  let shown = false;
  try {
    const result = await compactRequest(request as unknown as Request, {
      ...metadata, signal, count: transport.count, summarise: transport.summarise,
      load: () => ports.load(conversationId),
      save: async (localCompaction) => {
        signal?.throwIfAborted();
        await ports.save(conversationId, localCompaction);
      },
      status: (text) => {
        shown = true;
        ports.toast.loading(text, { id: toastId, duration: Infinity });
      },
    });
    if (shown) {
      ports.toast.success('Context summarised locally. Full chat history is retained.', {
        id: toastId, duration: 8000,
      });
    }
    return result as unknown as T;
  } catch (error) {
    if (shown) ports.toast.dismiss(toastId);
    throw error;
  }
}

export async function compactForChat<T extends { messages: unknown[]; model?: string }>(
  request: T,
  conversationId: string,
  signal?: AbortSignal,
): Promise<T> {
  const { toast } = await import('svelte-sonner');
  const { DatabaseService } = await import('$lib/services/database.service');
  const { getJsonHeaders } = await import('$lib/utils/api-headers');
  return compactForChatWith({
    fetch,
    headers: getJsonHeaders,
    load: async (id) => (await DatabaseService.getConversation(id))?.localCompaction,
    save: (id, localCompaction) => DatabaseService.updateConversation(id, { localCompaction }),
    toast,
  }, request, conversationId, signal);
}
