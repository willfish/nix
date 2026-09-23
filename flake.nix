{
  description = ''
    Configurations for my NixOS systems and Home Manager setup.

    - Andromeda is a NixOS configuration for my Thelio Major Threadripper desktop computer.
    - Starfish is a NixOS configuration for my Dell Precision 5750 laptop.
    - Foundation is a NixOS configuration for my Framework 13 AMD AI-300 Series laptop.
    - Terminus is a NixOS configuration for my Beelink NAS / headless host.
    - Home Manager configuration for my user account on all systems.
  '';
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    nix-config.url = "git+ssh://git@github.com/willfish/nix-config.git";
    agent-bus = {
      url = "git+ssh://git@github.com/willfish/pi-switchboard.git";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    nix-darwin = {
      url = "github:nix-darwin/nix-darwin/nix-darwin-26.05";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    pre-commit-hooks = {
      url = "github:cachix/git-hooks.nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    flake-parts = {
      url = "github:hercules-ci/flake-parts";
      inputs.nixpkgs-lib.follows = "nixpkgs";
    };
    treefmt-nix = {
      url = "github:numtide/treefmt-nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    stylix = {
      url = "github:nix-community/stylix/release-26.05";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    nix-index-database = {
      url = "github:nix-community/nix-index-database";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    home-manager = {
      url = "github:nix-community/home-manager/release-26.05";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    sops-nix = {
      url = "github:Mic92/sops-nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    sniffy = {
      url = "github:willfish/sniffy";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    smailer = {
      url = "github:willfish/smailer";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    mux = {
      url = "github:willfish/mux";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    herdr = {
      url = "github:ogulcancelik/herdr";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    forte = {
      url = "github:willfish/forte";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    walls = {
      url = "github:willfish/walls";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    llm-agents.url = "github:numtide/llm-agents.nix";
    hermes-agent.url = "github:NousResearch/hermes-agent/08a2e7dbccfc9aafbf6715965963d8b31347f37d";
    nixos-hardware.url = "github:NixOS/nixos-hardware/master";
    # Remote branch used only for packages under review (e.g. bootdev-cli PR).
    # Remove this input once the package is available in the pinned nixpkgs release.
    # nixpkgs-local.url = "github:willfish/nixpkgs/bootdev-cli-1.30.0";
    # Tailscale 1.102.3; nixos-26.05 is still on 1.98.10. Drop once the release
    # channel ships a current stable client.
    nixpkgs-unstable.url = "github:NixOS/nixpkgs/nixos-unstable";
  };
  outputs =
    inputs@{
      nixpkgs,
      pre-commit-hooks,
      flake-parts,
      treefmt-nix,
      stylix,
      nix-index-database,
      home-manager,
      sops-nix,
      nix-config,
      agent-bus,
      nix-darwin,
      sniffy,
      smailer,
      mux,
      herdr,
      forte,
      walls,
      llm-agents,
      nixos-hardware,
      # nixpkgs-local,
      nixpkgs-unstable,
      ...
    }:
    let
      linuxSystem = "x86_64-linux";
      darwinSystem = "aarch64-darwin";
      inherit (nixpkgs) lib;
      systems = [
        linuxSystem
        darwinSystem
      ];
      mkOverlay = system: _final: _prev: {
        inherit (sniffy.packages.${system}) sniffy;
        inherit (smailer.packages.${system}) smailer;
        mux = mux.packages.${system}.default;
        herdr = herdr.packages.${system}.default;
        forte = forte.packages.${system}.default;
        inherit (walls.packages.${system}) walls;
        pi-coding-agent = llm-agents.packages.${system}.pi;
        hermes-agent = import ./home/user/hermes-package.nix {
          hermesInput = inputs.hermes-agent;
          inherit system;
        };
        # Static Go client; safe to pull from unstable while 26.05 lags.
        inherit (nixpkgs-unstable.legacyPackages.${system}) tailscale;
      };
      mkPkgs =
        system:
        import nixpkgs {
          inherit system;
          config.allowUnfree = true;
          overlays = [
            (mkOverlay system)
          ];
        };
      pkgs = mkPkgs linuxSystem;
      darwinPkgs = mkPkgs darwinSystem;
      darwinHosts = {
        relay = ./system/darwin/relay.nix;
      };
      mkPreCommitCheck =
        system: pkgsFor:
        let
          fishSyntax = pkgsFor.writeShellApplication {
            name = "check-fish-syntax";
            runtimeInputs = [ pkgsFor.fish ];
            text = builtins.readFile ./scripts/check-fish-syntax;
          };
          eclintFiles = pkgsFor.writeShellApplication {
            name = "eclint-files";
            runtimeInputs = [ pkgsFor.eclint ];
            text = builtins.readFile ./scripts/eclint-files;
          };
        in
        pre-commit-hooks.lib.${system}.run {
          src = ./.;
          configPath = ".pre-commit-config-nix.yaml";
          hooks = {
            actionlint.enable = true;
            check-added-large-files = {
              enable = true;
              stages = lib.mkForce [ "pre-commit" ];
              # Intentional 638 KiB voice reference with adjacent provenance.
              excludes = [ "^home/config/voice/voices/samantha-reference\\.wav$" ];
            };
            check-case-conflicts.enable = true;
            check-json.enable = true;
            check-merge-conflicts.enable = true;
            check-yaml.enable = true;
            deadnix.enable = true;
            detect-private-keys.enable = true;
            eclint = {
              enable = true;
              entry = "${eclintFiles}/bin/eclint-files";
            };
            end-of-file-fixer.enable = true;
            flake-checker.enable = false;
            nil.enable = true;
            treefmt = {
              enable = true;
              package = treefmt-nix.lib.mkWrapper pkgsFor {
                projectRootFile = "flake.nix";
                programs = {
                  just.enable = true;
                  nixfmt.enable = true;
                  prettier = {
                    enable = true;
                    includes = [
                      "*.json"
                      "*.yaml"
                      "*.yml"
                    ];
                  };
                  shfmt.enable = true;
                  stylua.enable = true;
                };
                settings.formatter.fish = {
                  command = "${pkgsFor.fish}/bin/fish_indent";
                  includes = [ "*.fish" ];
                };
              };
            };
            shellcheck.enable = true;
            shellcheck.excludes = [ "^\\.envrc$" ];
            statix = {
              enable = true;
              settings.config = ".statix.toml";
            };
            trim-trailing-whitespace = {
              enable = true;
              stages = lib.mkForce [ "pre-commit" ];
            };

            fish-syntax = {
              enable = true;
              name = "fish-syntax";
              description = "Check Fish scripts parse correctly";
              entry = "${fishSyntax}/bin/check-fish-syntax";
              types = [ "text" ];
            };
          };
        };
      pre-commit-check = mkPreCommitCheck linuxSystem pkgs;
      darwin-pre-commit-check = mkPreCommitCheck darwinSystem darwinPkgs;
      homeModules = [
        stylix.homeModules.stylix
        nix-index-database.homeModules.default
        sops-nix.homeManagerModules.sops
        nix-config.homeModules.default
        agent-bus.homeManagerModules.default
        ./home
      ];
      nixosBaseModules = [
        sops-nix.nixosModules.sops
        nix-config.nixosModules.default
        {
          nixpkgs = {
            config.allowUnfree = true;
            overlays = [ (mkOverlay linuxSystem) ];
          };
        }
      ];
    in
    flake-parts.lib.mkFlake { inherit inputs; } {
      inherit systems;

      imports = [
        treefmt-nix.flakeModule
      ];

      flake = {
        overlays.default = final: prev: mkOverlay prev.stdenv.hostPlatform.system final prev;
        homeModules.default = {
          imports = [
            nix-config.homeModules.default
            agent-bus.homeManagerModules.default
            ./home
          ];
        };

        darwinConfigurations = lib.mapAttrs (
          name: module:
          nix-darwin.lib.darwinSystem {
            specialArgs = {
              homeConfiguration = inputs.self.homeConfigurations."william@${name}".config;
              sopsInstaller = sops-nix.packages.${darwinSystem}.sops-install-secrets;
            };
            modules = [
              { nixpkgs.pkgs = darwinPkgs; }
              sops-nix.darwinModules.sops
              nix-config.darwinModules.default
              module
            ];
          }
        ) darwinHosts;

        nixosConfigurations = {
          andromeda = lib.nixosSystem {
            system = linuxSystem;
            modules = nixosBaseModules ++ [ ./system/andromeda/configuration.nix ];
          };
          starfish = lib.nixosSystem {
            system = linuxSystem;
            modules = nixosBaseModules ++ [ ./system/starfish/configuration.nix ];
          };
          foundation = lib.nixosSystem {
            system = linuxSystem;
            modules = nixosBaseModules ++ [ ./system/foundation/configuration.nix ];
            specialArgs = {
              inherit nixos-hardware;
            };
          };
          terminus = lib.nixosSystem {
            system = linuxSystem;
            specialArgs.immichPkgs = nixpkgs-unstable.legacyPackages.${linuxSystem};
            modules = nixosBaseModules ++ [
              agent-bus.nixosModules.default
              ./system/terminus/configuration.nix
            ];
          };
        };

        homeConfigurations =
          let
            mkWilliamLinux =
              hostName:
              home-manager.lib.homeManagerConfiguration {
                inherit pkgs;
                modules = homeModules;
                extraSpecialArgs = lib.optionalAttrs (hostName != null) {
                  inherit hostName;
                };
              };
            williamLinux = mkWilliamLinux null;
            darwinHomes = lib.mapAttrs (
              hostName: _:
              home-manager.lib.homeManagerConfiguration {
                pkgs = darwinPkgs;
                modules = homeModules ++ [ { dotfiles.role = "automation"; } ];
                extraSpecialArgs = {
                  inherit hostName;
                  darwinConfig = inputs.self.darwinConfigurations.${hostName}.config;
                };
              }
            ) darwinHosts;
          in
          {
            william = williamLinux;
            william-linux = williamLinux;
            william-darwin = darwinHomes.relay;
            "william@foundation" = mkWilliamLinux "foundation";
            "william@starfish" = mkWilliamLinux "starfish";
            "william@andromeda" = mkWilliamLinux "andromeda";
            "william@terminus" = mkWilliamLinux "terminus";
          }
          // lib.mapAttrs' (name: home: lib.nameValuePair "william@${name}" home) darwinHomes;
      };

      perSystem =
        {
          config,
          pkgs,
          system,
          ...
        }:
        let
          preCommitCheck = if system == darwinSystem then darwin-pre-commit-check else pre-commit-check;
        in
        {
          _module.args.pkgs = mkPkgs system;

          packages.mcp-dap-server = pkgs.callPackage ./home/user/mcp-packages/mcp-dap-server.nix { };

          treefmt = {
            projectRootFile = "flake.nix";
            programs = {
              just.enable = true;
              nixfmt.enable = true;
              prettier = {
                enable = true;
                includes = [
                  "*.json"
                  "*.yaml"
                  "*.yml"
                ];
              };
              shfmt.enable = true;
              stylua.enable = true;
            };
            settings.formatter.fish = {
              command = "${pkgs.fish}/bin/fish_indent";
              includes = [ "*.fish" ];
            };
          };

          checks = {
            pi-agent-bus-composition = import ./tests/pi-agent-bus-composition.nix {
              inherit lib pkgs;
              home =
                inputs.self.homeConfigurations.${
                  if system == darwinSystem then "william-darwin" else "william-linux"
                }.config;
            };
            pi-agent-bus-runtime = agent-bus.lib.mkPiRuntimeCheck {
              inherit pkgs;
              piPackage = pkgs.pi-coding-agent;
              hubPackage =
                if system == linuxSystem then
                  inputs.self.nixosConfigurations.terminus.config.services.pi-agent-bus.package
                else
                  agent-bus.packages.${system}.hub;
              extensionPackage =
                inputs.self.homeConfigurations.${
                  if system == darwinSystem then "william-darwin" else "william-linux"
                }.config.programs.pi-agent-bus.package;
            };
            headless-darwin = import ./tests/headless-darwin.nix {
              inherit lib pkgs;
              darwin = inputs.self.darwinConfigurations.relay;
              home = inputs.self.homeConfigurations.william-darwin.config;
            };
            host-capabilities = import ./tests/host-capabilities.nix {
              inherit lib pkgs;
              homes = inputs.self.homeConfigurations;
            };
            pre-commit = preCommitCheck;
            home-profiles =
              let
                homes = inputs.self.homeConfigurations;
                server = homes."william@terminus".config;
                desktop = homes."william@andromeda".config;
                darwin = homes.william-darwin.config;
                hasNetcat = cfg: lib.any (p: lib.getName p == "netcat-openbsd") cfg.home.packages;
              in
              assert !server.programs.brave.enable;
              assert !server.programs.google-chrome.enable;
              assert !(server.systemd.user.services ? nm-auto-secret-agent);
              assert !(server.systemd.user.services ? wifi-auto-reconnect);
              assert !(server.home.activation ? importGraphicalSessionEnvironment);
              assert !(server.home.activation ? fixCosmicScreenshotPortalConfig);
              assert !(server.home.sessionVariables ? BROWSER);
              assert !(server.home.sessionVariables ? TERMINAL);
              assert !(lib.any (p: lib.getName p == "gimp") server.home.packages);
              assert lib.any (p: lib.getName p == "gimp") desktop.home.packages;
              assert !(server.xdg.configFile ? "cosmic/com.system76.CosmicComp/v1/autotile");
              assert server.home.file ? ".config/herdr/config.toml";
              assert desktop.programs.brave.enable && desktop.programs.google-chrome.enable;
              assert desktop.systemd.user.services ? nm-auto-secret-agent;
              assert desktop.home.sessionVariables.BROWSER == "brave";
              assert hasNetcat desktop && hasNetcat server && !hasNetcat darwin;
              pkgs.runCommand "home-profile-boundaries" { } "touch $out";
          }
          // lib.optionalAttrs (system == darwinSystem) {
            headless-browser =
              pkgs.runCommand "darwin-headless-browser"
                {
                  nativeBuildInputs = [ (pkgs.python3.withPackages (ps: [ ps.playwright ])) ];
                  CHROME_PATH = (import ./home/user/darwin-browser.nix { inherit pkgs; }).executable;
                }
                ''
                  export HOME="$TMPDIR/home"
                  mkdir -p "$HOME"
                  python3 ${./tests/darwin-headless-browser.py}
                  touch "$out"
                '';
          };

          devShells.default =
            let
              chromiumRevision = pkgs.playwright-driver.browsersJSON.chromium.revision;
              chromiumExecutable =
                if pkgs.stdenv.isDarwin then
                  (import ./home/user/darwin-browser.nix { inherit pkgs; }).executable
                else
                  "${pkgs.playwright-driver.browsers-chromium}/chromium-${chromiumRevision}/chrome-linux/chrome";
              mmdc = pkgs.writeShellScriptBin "mmdc" ''
                export PUPPETEER_SKIP_CHROMIUM_DOWNLOAD=1
                export PUPPETEER_EXECUTABLE_PATH=${chromiumExecutable}
                exec ${pkgs.mermaid-cli}/bin/mmdc "$@"
              '';
            in
            pkgs.mkShell {
              buildInputs =
                preCommitCheck.enabledPackages
                ++ [
                  config.treefmt.build.wrapper
                  mmdc
                ]
                ++ (with pkgs; [
                  bats
                  fish
                  mitmproxy
                  dbus
                  d2
                  just
                  nodejs
                  # Runtime parsers/HTTP client exercised by the local-assistant tests.
                  (python3.withPackages (ps: [
                    ps.pyyaml
                    ps.httpx
                    ps.beautifulsoup4
                    ps.jinja2
                    ps.dbus-next
                  ]))
                ]);
              shellHook =
                preCommitCheck.shellHook
                + ''
                  hook="$(git rev-parse --git-path hooks/pre-push 2>/dev/null || true)"
                  if [ -n "$hook" ] && [ -L "$hook" ] && [ "$(basename "$(readlink "$hook")")" = dotfiles-pre-push ]; then
                    rm -f "$hook"
                  fi
                ''
                + lib.optionalString (system == darwinSystem) ''
                  echo "Diagram tools available: d2, mmdc, nodejs"
                '';
            };
        };
    };
}
