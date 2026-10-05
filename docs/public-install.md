# Install the shared home on another laptop

This entry point installs the shared Home Manager desktop and development configuration for the current user. It does not require William's SSH identity, private flake inputs or MCP credentials. It does not repartition disks, install NixOS, create a user or activate one of the machine-specific NixOS configurations.

## Prerequisites

Use a normal user account with a writable home, Git, Bash and a working [Nix installation](https://nixos.org/download/). Run the installer as that user, not through `sudo`. Downloads require internet access and the desktop closure needs substantial disk space. Some packages are unfree.

The isolated installation test targets x86_64 Linux. Home Manager supplies user configuration, not the host's display manager, graphics drivers, NetworkManager, audio or system services. On non-NixOS distributions, graphical applications may also need graphics integration such as nixGL. Docker verifies build and activation, not a graphical login or hardware support.

## Install

```sh
git clone https://github.com/willfish/nix.git ~/.dotfiles
cd ~/.dotfiles
bash scripts/install-home --dry  # Build without changing the home
bash scripts/install-home        # Build and activate
```

No Home Manager CLI or direnv bootstrap is required. The script enables the necessary Nix experimental features for its own commands and supplies the current username and home directory. A laptop whose hostname happens to match William's machine does not acquire that machine's private services.

On activation, unmanaged shell files such as `.bashrc` and `.profile` are backed up beside the originals with a printed `before-dotfiles-<timestamp>-<pid>` suffix. Existing backups are not overwritten. Keep these files if you need to restore the previous shell setup. The configuration also manages desktop and agent settings, so review the modules before adopting it over an existing customised home.

Use `--public` to skip credential detection entirely:

```sh
bash scripts/install-home --public
```

After installation, open a new shell and use `hmswitch`, or run `~/.bin/hmswitch` directly. The installer remembers the checkout location, including a checkout outside `~/.dotfiles`. `HMSWITCH_FLAKE=/path/to/checkout` overrides it. Public installations support `--dry` and `--public`; authenticated owner installations retain the existing `nh` options.

## What is disabled without credentials?

The public home retains the desktop, terminal, editor, development tools and Pi. It excludes owner account integrations, private skills and guides, secret declarations, Git identity/signing defaults, owner model credentials and agent-bus integration. Credential-dependent MCPs and William's SSH-backed arXiv service are not registered. Local browser, filesystem, debugger and NixOS MCP tooling remain available.

Pi still needs the new user's own provider login or API key before it can call a model. Existing user-managed credentials are not revoked by switching to public mode.

Set your own Git identity in the writable `~/.gitconfig`, which overrides the managed XDG Git configuration:

```sh
git config --file ~/.gitconfig user.name 'Your Name'
git config --file ~/.gitconfig user.email 'you@example.com'
```

Do not edit `~/.config/git/config` directly; Home Manager owns that file.

## Automatic credential detection

The installer looks for `~/.ssh/id_ed25519`. Without that file, it never evaluates the private input. If a key exists, it attempts noninteractive access to the pinned `nix-config` source, derives an age identity from the SSH key and verifies decryption of the pinned encrypted environment file.

Private integrations are enabled only after that decryption succeeds. An unrelated key, unavailable private repository or failed decryption selects public mode. The probe ignores ambient decryption credentials, writes no plaintext secrets, and deletes its temporary derived key. Merely having an SSH key or an existing secret directory is insufficient.

The installer also checks access to the pinned agent-bus input before selecting private composition. The named `william-*` and `william@*` outputs remain owner configurations. Do not use them to install another user's home.

## Reuse from another flake

`lib.mkHome` is public by default:

```nix
homeConfigurations.alex = dotfiles.lib.mkHome {
  username = "alex";
  homeDirectory = "/home/alex";
  system = "x86_64-linux";
};
```

Set `sourceDirectory` if the checkout is not at `~/.dotfiles`; runtime theme builds use it. Pass additional Home Manager modules with `modules = [ ./home.nix ];`. Private detection belongs to the installer, not pure Nix evaluation. Setting `privateEnabled = true` explicitly opts into the private module composition and its access requirements.

Root-wide `nix flake show` and lock-file updates can evaluate or fetch owner inputs. They are not the public installation entry point. Use `scripts/install-home --public --dry` to build the public home for the current identity without activating it. Private-access detection and installation remain explicit runtime boundaries; there is no flake test or clean-room test runner.
