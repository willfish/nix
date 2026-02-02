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
    nixos-hardware.url = "github:NixOS/nixos-hardware/master";
    nix-clawdbot = {
      url = "github:moltbot/nix-clawdbot";
      inputs.nixpkgs.follows = "nixpkgs-unstable";
    };
    nixpkgs-local.url = "path:/home/william/Repositories/nixpkgs";
  };
  outputs =
    {
      nixpkgs-unstable,
      pre-commit-hooks,
      home-manager,
      sniffy,
      smailer,
      mux,
      nixos-hardware,
      nix-clawdbot,
      nixpkgs-local,
      ...
    }:
    let
      system = "x86_64-linux";
      lib = nixpkgs-unstable.lib;
      pkgs-local = import nixpkgs-local { inherit system; };
      overlay = (
        final: prev: {
          inherit (sniffy.packages.${system}) sniffy;
          inherit (smailer.packages.${system}) smailer;
          mux = mux.packages.${system}.default;
          variety = pkgs-local.variety;
        }
      );
      pkgs = import nixpkgs-unstable {
        inherit system;
        config.allowUnfree = true;
        config.nvidia.acceptLicense = true;
        overlays = [
          overlay
          nix-clawdbot.overlays.default
          # The upstream gateway install script omits docs/reference/templates,
          # but dist/agents/workspace.js resolves templates relative to itself.
          # Patch gateway and propagate to the batteries bundle.
          (final: prev: {
            clawdbot-gateway = prev.clawdbot-gateway.overrideAttrs (old: {
              postFixup = (old.postFixup or "") + ''
                templates=$(find $out/lib/clawdbot/node_modules/.pnpm \
                  -path '*/node_modules/clawdbot/docs/reference/templates' -type d | head -1)
                if [ -n "$templates" ]; then
                  mkdir -p $out/lib/clawdbot/docs/reference
                  cp -r "$templates" $out/lib/clawdbot/docs/reference/templates
                fi
              '';
            });
            clawdbot = prev.clawdbot.override {
              clawdbot-gateway = final.clawdbot-gateway;
            };
          })
        ];
      };
      pre-commit-check = pre-commit-hooks.lib.${system}.run {
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
          inherit system;
          modules = [ ./system/andromeda/configuration.nix ];
          specialArgs = {
            pkgs-unstable = pkgs;
          };
        };
        starfish = lib.nixosSystem {
          inherit system;
          modules = [ ./system/starfish/configuration.nix ];
          specialArgs = {
            pkgs-unstable = pkgs;
          };
        };
        foundation = lib.nixosSystem {
          inherit system;
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
          modules = [
            nix-clawdbot.homeManagerModules.clawdbot
            ./home
          ];
        };
      };
      devShells.${system} = {
        default = pkgs.mkShell {
          inherit (pre-commit-check) shellHook;
          buildInputs = pre-commit-check.enabledPackages;
        };
      };
    };
}
