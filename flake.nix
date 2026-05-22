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
    pre-commit-hooks = {
      url = "github:cachix/git-hooks.nix";
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
    trade-tariff-tools = {
      url = "github:trade-tariff/trade-tariff-tools";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    nixos-hardware.url = "github:NixOS/nixos-hardware/master";
    # nixpkgs-local.url = "path:/home/william/Repositories/nixpkgs";
  };
  outputs =
    {
      nixpkgs,
      pre-commit-hooks,
      home-manager,
      sniffy,
      smailer,
      mux,
      forte,
      trade-tariff-tools,
      nixos-hardware,
      # nixpkgs-local,
      ...
    }:
    let
      linuxSystem = "x86_64-linux";
      darwinSystem = "aarch64-darwin";
      lib = nixpkgs.lib;
      linuxOverlay = (
        _final: _prev: {
          inherit (sniffy.packages.${linuxSystem}) sniffy;
          inherit (smailer.packages.${linuxSystem}) smailer;
          mux = mux.packages.${linuxSystem}.default;
          forte = forte.packages.${linuxSystem}.default;
          inherit (trade-tariff-tools.packages.${linuxSystem}) ecs;
        }
      );
      darwinOverlay = (
        _final: _prev: {
          inherit (sniffy.packages.${darwinSystem}) sniffy;
          inherit (smailer.packages.${darwinSystem}) smailer;
          mux = mux.packages.${darwinSystem}.default;
          inherit (trade-tariff-tools.packages.${darwinSystem}) ecs;
        }
      );
      pkgs = import nixpkgs {
        system = linuxSystem;
        config.allowUnfree = true;
        config.nvidia.acceptLicense = true;
        overlays = [
          linuxOverlay
        ];
      };
      darwinPkgs = import nixpkgs {
        system = darwinSystem;
        config.allowUnfree = true;
        overlays = [
          darwinOverlay
        ];
      };
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
            nixfmt.enable = true;
            shellcheck.enable = true;
            shellcheck.excludes = [ "^\\.envrc$" ];
            shfmt.enable = true;
            stylua.enable = true;
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
    in
    {
      nixosConfigurations = {
        andromeda = lib.nixosSystem {
          system = linuxSystem;
          modules = [ ./system/andromeda/configuration.nix ];
        };
        starfish = lib.nixosSystem {
          system = linuxSystem;
          modules = [ ./system/starfish/configuration.nix ];
        };
        foundation = lib.nixosSystem {
          system = linuxSystem;
          modules = [ ./system/foundation/configuration.nix ];
          specialArgs = {
            inherit nixos-hardware;
          };
        };
      };
      homeConfigurations =
        let
          williamLinux = home-manager.lib.homeManagerConfiguration {
            inherit pkgs;
            modules = [ ./home ];
            extraSpecialArgs = {
              enableCuda = false;
              isLinux = true;
            };
          };
          williamDarwin = home-manager.lib.homeManagerConfiguration {
            pkgs = darwinPkgs;
            modules = [ ./home ];
            extraSpecialArgs = {
              enableCuda = false;
              isLinux = false;
            };
          };
        in
        {
          william = williamDarwin;
          william-linux = williamLinux;
          william-darwin = williamDarwin;
          "william@foundation" = williamLinux;
          "william@starfish" = williamLinux;
          "william@andromeda" = home-manager.lib.homeManagerConfiguration {
            inherit pkgs;
            modules = [ ./home ];
            extraSpecialArgs = {
              enableCuda = true;
              isLinux = true;
            };
          };
        };
      devShells.${linuxSystem} = {
        default = pkgs.mkShell {
          inherit (pre-commit-check) shellHook;
          buildInputs =
            pre-commit-check.enabledPackages
            ++ (with pkgs; [
              d2
              nodejs
              (writeShellScriptBin "mmdc" ''
                export PUPPETEER_SKIP_CHROMIUM_DOWNLOAD=1
                export PUPPETEER_EXECUTABLE_PATH=${chromium}/bin/chromium
                exec ${pkgs.mermaid-cli}/bin/mmdc "$@"
              '')
            ]);
        };
      };
      devShells.${darwinSystem} = {
        default = darwinPkgs.mkShell {
          buildInputs =
            darwin-pre-commit-check.enabledPackages
            ++ (with darwinPkgs; [
              d2
              nodejs
            ]);
          shellHook = ''
            ${darwin-pre-commit-check.shellHook}
            # mmdc is available via npx (best experience on Apple Silicon)
            # Run: npx @mermaid-js/mermaid-cli --help
            echo "Diagram tools available: d2, nodejs (use npx @mermaid-js/mermaid-cli for mmdc)"
          '';
        };
      };
    };
}
