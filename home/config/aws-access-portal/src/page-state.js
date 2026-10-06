(() => {
  const alert = document.querySelector('[data-testid="test-alert"]');
  return {
    href: location.href,
    username: !!document.querySelector('#username-input'),
    password: !!document.querySelector('[data-testid="test-password-input"]'),
    mfa: !!document.querySelector('[data-testid="vmfa-authentication"] [data-testid="test-input"]'),
    accounts: !!document.querySelector('[data-testid="accounts"]') || !!document.querySelector('[data-testid="account-list-cell"]'),
    alert: alert ? alert.innerText.slice(0, 180) : '',
    focused: document.hasFocus()
  };
})()
