# Nix Ephemeral Tooling

When a task needs a CLI that is not already available, use Nix ephemerally.

Preferred patterns:

```sh
nix shell nixpkgs#<package> -c <command>
nix-shell -p <package>
```

Do not add tools to Home Manager packages, overlays, flake inputs, language
manifests, or project config just to satisfy a temporary agent task.

Do not use host-level installers such as `apt`, `brew`, `npm install -g`,
`pip install --user`, or similar unless Will explicitly asks for a persistent
installation.

For dotfiles/Home Manager changes, verify with the relevant activation package,
for example:

```sh
nix build .#homeConfigurations.william-linux.activationPackage
```
