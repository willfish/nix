# Flake lock update policy

`check-flake-lock-update BASE HEAD` validates both complete lock graphs before
allowing changes. It rejects duplicate JSON keys, unsupported source attributes,
unresolved references and follows cycles. Ordinary dependency cycles and shared
nodes remain legal. Unreachable nodes receive the same checks.

Only existing GitHub revision/hash/timestamp fields may change. Graph and source
identity remain fixed; approval comes from exact lines in
`AUTO_MERGE_GITHUB_OWNERS`, with no default allowlist. Rejections return 1 with
`Manual review required:`; accepted changes are sorted in the success message.

Build from trusted code with `nix build .#check-flake-lock-update`, not by
evaluating an untrusted pull request's flake. The command grants no merge
authorization and is not an automatic hook. Manual checks are retained with the
[repository command collection](../collections/repo-tools).
