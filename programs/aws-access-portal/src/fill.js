((field, button, value) => {
  if (location.origin !== 'https://d-9c677042e2.awsapps.com' &&
      location.origin !== 'https://eu-west-2.signin.aws') return false;
  const fieldElement = document.querySelector(field);
  const buttonElement = document.querySelector(button);
  const el = fieldElement?.matches('input') ? fieldElement : fieldElement?.querySelector('input');
  const submit = buttonElement?.matches('button, input[type="submit"]')
    ? buttonElement : buttonElement?.querySelector('button, input[type="submit"]');
  if (!el || !submit) return false;
  const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set;
  setter.call(el, value);
  el.dispatchEvent(new InputEvent('input', {bubbles: true, inputType: 'insertText', data: value}));
  el.dispatchEvent(new Event('change', {bubbles: true}));
  submit.click();
  return true;
})
