# shellcheck shell=bash
# The fallback is independent of both resident services.
if [ "${1:-}" = "--fallback" ]; then
  shift
  exec hypr-launcher-fallback "$@"
fi

hypr-theme-seed
if timeout 3s systemctl --user start elephant.service walker.service &&
  timeout 1s elephant query 'desktopapplications;;1' >/dev/null 2>&1 &&
  timeout 2s walker "$@"; then
  exit 0
fi

printf 'hypr-launcher: Walker unavailable; opening Fuzzel instead\n' >&2
exec hypr-launcher-fallback
