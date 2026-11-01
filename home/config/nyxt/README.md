# Nyxt profile

Home Manager installs this profile on Linux desktops. Brave remains the default
browser and is not modified.

Treat this as a trial browser: the current CEF build has emitted graphics-process
crash diagnostics even when browsing recovers. Keep Brave for reliable browsing.

The profile uses native Vi page navigation, Google search (`g` shortcut), no live
search suggestions, UK English spelling, and the same GPU video-decoding
workaround as Brave. The address prompt retains its normal editing bindings.
Nyxt's CEF adblocker remains enabled.

## Blocking lists: partial compatibility only

`filter-lists.json` records Brave's default lists and the enabled optional lists:
annoyances, cookies, AI suggestions, newsletters, mobile app promotions, social
widgets, YouTube distractions, tracking URLs, chat widgets and paywall filters.
Their subscription URLs are added to Nyxt's existing defaults. Filter contents
are downloaded at runtime and cached for four days; stale cached copies are kept
when an update fails. Missing uncached lists produce startup warnings. This is a
snapshot of the selected subscriptions, not live synchronization with Brave.

The current CEF blocker skips scriptlets, procedural cosmetic selectors and
tracking-parameter removal. Those parts of the imported lists do not work, and
paywall-filter behaviour is not equivalent to Brave. Brave's fingerprinting
protections are not reproduced either. Lists load asynchronously, so requests
can be allowed before initial downloading completes. Keep using Brave when these
protections matter.

Full uBlock Origin is deliberately not installed: it did not provide working
filtering in the current Nyxt/CEF integration. Importing the same list URLs is
not a claim of equivalent protection.

## Bitwarden

At startup, Nyxt locates the newest installed Bitwarden extension under Brave's
Default profile and loads only its extension code. It never reads or imports
Brave's extension settings, vault, tokens, cookies or sessions. Nyxt uses separate
storage. Sign in manually, and use `Ctrl+Shift+U` or `open-bitwarden` to open it.
If Brave does not have Bitwarden installed, the other settings still work.

Autofill and password-generation shortcuts are not remapped or verified by the
profile. They need a manual check after signing in. Restart Nyxt after Brave
updates the extension.

## Differences

- Native Vi mode replaces Vimium C; it does not reproduce every Vimium binding
  or its non-ASCII keyboard-layout rule.
- Collapsed vertical tabs and Brave's close-window confirmation are not ported.
  Use Nyxt's buffer-switching commands; window closing follows Nyxt defaults.
- Browser sync, bookmarks, navigation marks and history are not migrated.
- No MCP server, remote Lisp execution or debugging port is enabled. A dedicated
  Nyxt MCP server has not been selected; CEF debugging and Nyxt remote execution
  need a separate access-control decision before agent integration.
