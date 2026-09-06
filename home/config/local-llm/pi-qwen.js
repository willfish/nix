// Pi handles enable_thinking; Qwen 3.8 also needs its level inside the template kwargs.
export default function localQwen(pi) {
  pi.on('before_provider_request', (event, ctx) => {
    if (ctx.model?.provider !== 'relay') return;
    const payload = event.payload;
    const thinking = payload.chat_template_kwargs?.enable_thinking === true;
    const level = pi.getThinkingLevel();
    const effort = level === 'high' || level === 'xhigh' ? 'xhigh'
      : level === 'medium' ? 'medium' : 'low';
    return {
      ...payload,
      chat_template_kwargs: {
        ...payload.chat_template_kwargs,
        enable_thinking: thinking,
        preserve_thinking: true,
        ...(thinking ? { reasoning_effort: effort } : {}),
      },
      temperature: thinking ? 1.0 : 0.7,
      top_p: thinking ? 0.95 : 0.8,
      top_k: 20,
      min_p: 0,
      presence_penalty: thinking ? 0 : 1.5,
      repeat_penalty: 1,
    };
  });
}
