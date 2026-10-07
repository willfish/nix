# Color contrast

`contrast` checks an opaque six-digit sRGB pair against an unrounded WCAG
contrast threshold. It does not evaluate composited colors or establish the
accessibility of an entire interface.

Build with `nix build .#color-contrast`; the existing command remains `contrast`.
Home Manager also inserts it into the public design skill. Manual fixtures live
with the [native skill command collection](../collections/skill-tools).
