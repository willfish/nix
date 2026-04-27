{
  description = ''
    Configurations for my NixOS systems and Home Manager setup.

    - Andromeda is a NixOS configuration for my Thelio Major Threadripper desktop computer.
    - Starfish is a NixOS configuration for my Dell Precision 5750 laptop.
    - Foundation is a NixOS configuration for my Framework 13 AMD AI-300 Series laptop.
    - Home Manager configuration for my user account on all systems.
  '';
  inputs = {
    nixpkgs-unstable.url = "nixpkgs/nixos-unstable";
    pre-commit-hooks.url = "github:cachix/git-hooks.nix";
    home-manager = {
      url = "github:nix-community/home-manager";
      inputs.nixpkgs.follows = "nixpkgs-unstable";
    };
    sniffy = {
      url = "github:willfish/sniffy";
      inputs.nixpkgs.follows = "nixpkgs-unstable";
    };
    smailer = {
      url = "github:willfish/smailer";
      inputs.nixpkgs.follows = "nixpkgs-unstable";
    };
    mux = {
      url = "github:willfish/mux";
      inputs.nixpkgs.follows = "nixpkgs-unstable";
    };
    forte = {
      url = "github:willfish/forte";
      inputs.nixpkgs.follows = "nixpkgs-unstable";
    };
    trade-tariff-tools = {
      url = "github:trade-tariff/trade-tariff-tools";
      inputs.nixpkgs.follows = "nixpkgs-unstable";
    };
    llm-agents = {
      url = "github:numtide/llm-agents.nix";
      inputs.nixpkgs.follows = "nixpkgs-unstable";
    };
    nixos-hardware.url = "github:NixOS/nixos-hardware/master";
    # nixpkgs-local.url = "path:/home/william/Repositories/nixpkgs";
  };
  outputs =
    {
      nixpkgs-unstable,
      pre-commit-hooks,
      home-manager,
      sniffy,
      smailer,
      mux,
      forte,
      trade-tariff-tools,
      llm-agents,
      nixos-hardware,
      # nixpkgs-local,
      ...
    }:
    let
      linuxSystem = "x86_64-linux";
      darwinSystem = "aarch64-darwin";
      lib = nixpkgs-unstable.lib;
      # pkgs-local = import nixpkgs-local { inherit system; };
      cliHelpersPygmentsTestsOverlay = (
        final: prev: {
          # cli-helpers 2.10.0 has brittle ANSI-colour expectations that fail
          # with Pygments 2.20.0, blocking pgcli/home-manager builds.
          # Keep checks enabled and skip only the affected style rendering tests.
          pythonPackagesExtensions = prev.pythonPackagesExtensions ++ [
            (python-final: python-prev: {
              cli-helpers = python-prev.cli-helpers.overridePythonAttrs (old: {
                disabledTests = (old.disabledTests or [ ]) ++ [
                  "test_style_output"
                  "test_style_output_with_newlines"
                  "test_style_output_custom_tokens"
                ];
              });
            })
          ];
        }
      );
      linuxOverlay = (
        final: prev: {
          inherit (sniffy.packages.${linuxSystem}) sniffy;
          inherit (smailer.packages.${linuxSystem}) smailer;
          mux = mux.packages.${linuxSystem}.default;
          forte = forte.packages.${linuxSystem}.default;
          inherit (trade-tariff-tools.packages.${linuxSystem}) ecs;
          inherit (llm-agents.packages.${linuxSystem}) codex claude-code;
          # variety = pkgs-local.variety;
        }
      );
      darwinOverlay = (
        final: prev: {
          inherit (sniffy.packages.${darwinSystem}) sniffy;
          inherit (smailer.packages.${darwinSystem}) smailer;
          mux = mux.packages.${darwinSystem}.default;
          inherit (trade-tariff-tools.packages.${darwinSystem}) ecs;
        }
      );
      pkgs = import nixpkgs-unstable {
        system = linuxSystem;
        config.allowUnfree = true;
        config.nvidia.acceptLicense = true;
        overlays = [
          cliHelpersPygmentsTestsOverlay
          linuxOverlay
        ];
      };
      darwinPkgs = import nixpkgs-unstable {
        system = darwinSystem;
        config.allowUnfree = true;
        overlays = [
          cliHelpersPygmentsTestsOverlay
          darwinOverlay
        ];
      };
      pre-commit-check = pre-commit-hooks.lib.${linuxSystem}.run {
        src = ./.;
        configPath = ".pre-commit-config-nix.yaml";
        hooks = {
          eclint.enable = true;
          end-of-file-fixer.enable = true;
          flake-checker.enable = false;
          nil.enable = true;
          ormolu.enable = true;
          trim-trailing-whitespace.enable = true;
        };
      };
    in
    {
      nixosConfigurations = {
        andromeda = lib.nixosSystem {
          system = linuxSystem;
          modules = [ ./system/andromeda/configuration.nix ];
          specialArgs = {
            pkgs-unstable = pkgs;
          };
        };
        starfish = lib.nixosSystem {
          system = linuxSystem;
          modules = [ ./system/starfish/configuration.nix ];
          specialArgs = {
            pkgs-unstable = pkgs;
          };
        };
        foundation = lib.nixosSystem {
          system = linuxSystem;
          modules = [ ./system/foundation/configuration.nix ];
          specialArgs = {
            pkgs-unstable = pkgs;
            inherit nixos-hardware;
          };
        };
      };
      homeConfigurations = {
        william = home-manager.lib.homeManagerConfiguration {
          inherit pkgs;
          modules = [ ./home ];
          extraSpecialArgs = {
            enableCuda = false;
            isLinux = true;
          };
        };
        "william@foundation" = home-manager.lib.homeManagerConfiguration {
          inherit pkgs;
          modules = [ ./home ];
          extraSpecialArgs = {
            enableCuda = false;
            isLinux = true;
          };
        };
        "william@starfish" = home-manager.lib.homeManagerConfiguration {
          inherit pkgs;
          modules = [ ./home ];
          extraSpecialArgs = {
            enableCuda = false;
            isLinux = true;
          };
        };
        "william@andromeda" = home-manager.lib.homeManagerConfiguration {
          inherit pkgs;
          modules = [ ./home ];
          extraSpecialArgs = {
            enableCuda = true;
            isLinux = true;
          };
        };
        william-darwin = home-manager.lib.homeManagerConfiguration {
          pkgs = darwinPkgs;
          modules = [ ./home ];
          extraSpecialArgs = {
            enableCuda = false;
            isLinux = false;
          };
        };
      };
      devShells.${linuxSystem} = {
        default = pkgs.mkShell {
          inherit (pre-commit-check) shellHook;
          buildInputs = pre-commit-check.enabledPackages;
        };
      };
      devShells.${darwinSystem} = {
        default = darwinPkgs.mkShell {
          buildInputs = [ ];
        };
      };
    };
}
