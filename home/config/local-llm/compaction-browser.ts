import { toast } from 'svelte-sonner';
import { DatabaseService } from '$lib/services/database.service';
import { getJsonHeaders } from '$lib/utils/api-headers';
import { compactRequest, type Request } from './compaction';
import { createTransport } from './compaction-transport';

/** Used only for conversation requests, never title generation or API clients. */
export async function compactForChat<T extends { messages: unknown[]; model?: string }>(
  request: T,
  conversationId: string,
  signal?: AbortSignal,
): Promise<T> {
  const transport = createTransport(fetch, getJsonHeaders, signal);
  const metadata = await transport.metadata(request.model);
  const toastId = `local-compaction-${conversationId}`;
  let shown = false;
  try {
    const result = await compactRequest(request as unknown as Request, {
      ...metadata, signal, count: transport.count, summarise: transport.summarise,
      load: async () => (await DatabaseService.getConversation(conversationId))?.localCompaction,
      save: async (localCompaction) => {
        signal?.throwIfAborted();
        await DatabaseService.updateConversation(conversationId, { localCompaction });
      },
      status: (text) => {
        shown = true;
        toast.loading(text, { id: toastId, duration: Infinity });
      },
    });
    if (shown) toast.success('Context summarised locally. Full chat history is retained.', { id: toastId, duration: 8000 });
    return result as unknown as T;
  } catch (error) {
    if (shown) toast.dismiss(toastId);
    throw error;
  }
}
