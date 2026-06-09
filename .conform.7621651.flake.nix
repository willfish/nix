{
  description = ''
    Configurations for my NixOS systems and Home Manager setup.

    - Andromeda is a NixOS configuration for my Thelio Major Threadripper desktop computer.
    - Starfish is a NixOS configuration for my Dell Precision 5750 laptop.
    - Foundation is a NixOS configuration for my Framework 13 AMD AI-300 Series laptop.
    - Home Manager configuration for my user account on all systems.
  '';
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.11";
    # Keep Mullvad at least as new as the running generation; older daemons
    # cannot parse newer settings and can fall back to a blocking firewall.
    nixpkgs-mullvad.url = "github:NixOS/nixpkgs/nixos-unstable";
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
      url = "github:nix-community/stylix/release-25.11";
      inputs.nixpkgs.follows = "nixpkgs";
      inputs.gnome-shell = {
        url = "github:GNOME/gnome-shell/gnome-49";
        flake = false;
      };
    };
    nix-index-database = {
      url = "github:nix-community/nix-index-database";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    home-manager = {
      url = "github:nix-community/home-manager/release-25.11";
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
    forte = {
      url = "github:willfish/forte";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    walls = {
      url = "github:willfish/walls";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    trade-tariff-tools = {
      url = "github:trade-tariff/trade-tariff-tools";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    llm-agents.url = "github:numtide/llm-agents.nix";
    nixos-hardware.url = "github:NixOS/nixos-hardware/master";
    # nixpkgs-local.url = "path:/home/william/Repositories/nixpkgs";
  };
  outputs =
    inputs@{
      nixpkgs,
      nixpkgs-mullvad,
      pre-commit-hooks,
      flake-parts,
      treefmt-nix,
      stylix,
      nix-index-database,
      home-manager,
      sniffy,
      smailer,
      mux,
      forte,
      walls,
      trade-tariff-tools,
      llm-agents,
      nixos-hardware,
      # nixpkgs-local,
      ...
    }:
    let
      linuxSystem = "x86_64-linux";
      darwinSystem = "aarch64-darwin";
      lib = nixpkgs.lib;
      systems = [
        linuxSystem
        darwinSystem
      ];
      mkOverlay =
        system: _final: prev:
        let
          mullvadPkgs = import nixpkgs-mullvad {
            inherit system;
            config.allowUnfree = true;
          };
        in
        {
          inherit (sniffy.packages.${system}) sniffy;
          inherit (smailer.packages.${system}) smailer;
          mux = mux.packages.${system}.default;
          forte = forte.packages.${system}.default;
          inherit (walls.packages.${system}) walls;
          inherit (trade-tariff-tools.packages.${system}) ecs;
          inherit (llm-agents.packages.${system})
            antigravity-cli
            claude-code
            codex
            grok
            ;
        }
        // lib.optionalAttrs (system == linuxSystem) {
          mullvad-vpn = mullvadPkgs.mullvad-vpn;

          variety = prev.variety.overrideAttrs (old: {
            postPatch = (old.postPatch or "") + ''
              substituteInPlace variety/Util.py \
                --replace-fail "super(VarietyMetadata, self).__init__(path=path)" \
                               "super(VarietyMetadata, self).__init__(); self.open_path(path)"
            '';
          });
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
      mkPreCommitCheck =
        system: pkgsFor:
        pre-commit-hooks.lib.${system}.run {
          src = ./.;
          configPath = ".pre-commit-config-nix.yaml";
          hooks = {
            actionlint.enable = true;
            check-added-large-files.enable = true;
            check-case-conflicts.enable = true;
            check-json.enable = true;
            check-merge-conflicts.enable = true;
            check-yaml.enable = true;
            deadnix.enable = true;
            detect-private-keys.enable = true;
            eclint.enable = true;
            end-of-file-fixer.enable = true;
            flake-checker.enable = false;
            nil.enable = true;
            treefmt = {
              enable = true;
              package = treefmt-nix.lib.mkWrapper pkgsFor {
                projectRootFile = "flake.nix";
                programs = {
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
            trim-trailing-whitespace.enable = true;

            fish-syntax = {
              enable = true;
              name = "fish-syntax";
              description = "Check Fish scripts parse correctly";
              entry = "${pkgsFor.fish}/bin/fish --no-config --no-execute";
              files = "^home/config/bin/(log_for|notes|notes_on)$";
            };
          };
        };
      pre-commit-check = mkPreCommitCheck linuxSystem pkgs;
      darwin-pre-commit-check = mkPreCommitCheck darwinSystem darwinPkgs;
      homeModules = [
        stylix.homeModules.stylix
        nix-index-database.homeModules.default
        ./home
      ];
      nixosBaseModules = [
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
        homeModules.default = ./home;

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
        };

        homeConfigurations =
          let
            williamLinux = home-manager.lib.homeManagerConfiguration {
              inherit pkgs;
              modules = homeModules;
              extraSpecialArgs = {
                isLinux = true;
              };
            };
            williamDarwin = home-manager.lib.homeManagerConfiguration {
              pkgs = darwinPkgs;
              modules = homeModules;
              extraSpecialArgs = {
                isLinux = false;
              };
            };
          in
          {
            william = williamLinux;
            william-linux = williamLinux;
            william-darwin = williamDarwin;
            "william@foundation" = williamLinux;
            "william@starfish" = williamLinux;
            "william@andromeda" = home-manager.lib.homeManagerConfiguration {
              inherit pkgs;
              modules = homeModules;
              extraSpecialArgs = {
                isLinux = true;
              };
            };
          };
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

          treefmt = {
            projectRootFile = "flake.nix";
            programs = {
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

          checks.pre-commit = preCommitCheck;

          devShells.default = pkgs.mkShell {
            buildInputs =
              preCommitCheck.enabledPackages
              ++ [
                config.treefmt.build.wrapper
              ]
              ++ (with pkgs; [
                d2
                nodejs
              ])
              ++ lib.optionals (system == linuxSystem) [
                (pkgs.writeShellScriptBin "mmdc" ''
                  export PUPPETEER_SKIP_CHROMIUM_DOWNLOAD=1
                  export PUPPETEER_EXECUTABLE_PATH=${pkgs.chromium}/bin/chromium
                  exec ${pkgs.mermaid-cli}/bin/mmdc "$@"
                '')
              ];
            shellHook =
              preCommitCheck.shellHook
              + lib.optionalString (system == darwinSystem) ''
                # mmdc is available via npx (best experience on Apple Silicon)
                # Run: npx @mermaid-js/mermaid-cli --help
                echo "Diagram tools available: d2, nodejs (use npx @mermaid-js/mermaid-cli for mmdc)"
              '';
          };
        };
    };
}
