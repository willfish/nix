{
  description = ''
    Andromeda and Starfish NixOS configurations
    Andromeda is a NixOS configuration for my Thelio Major Threadripper desktop computer.
    Starfish is a NixOS configuration for my Dell Precision 5750 laptop.
    Home Manager configuration for my user account on all systems.
  '';
  inputs = {
    nixpkgs.url = "nixpkgs/nixos-24.11";
    nixpkgs-unstable.url = "nixpkgs/nixos-unstable";
    pre-commit-hooks.url = "github:cachix/git-hooks.nix";
    home-manager = {
      url = "github:nix-community/home-manager/release-24.11";
      inputs.nixpkgs.follows = "nixpkgs-unstable";
    };
    sniffy = {
      url = "github:willfish/sniffy";
      inputs.nixpkgs.follows = "nixpkgs-unstable"; # Match home-manager
    };
  };
  outputs =
    { nixpkgs
    , nixpkgs-unstable
    , pre-commit-hooks
    , home-manager
    , sniffy
    , ...
    }:
      let
        system = "x86_64-linux";
        lib = nixpkgs.lib;
        pkgs = import nixpkgs {
          inherit system;
          config.allowUnfree = true;
          config.nvidia.acceptLicense = true;
        };
        pkgs-unstable = import nixpkgs-unstable {
          inherit system;
          config.allowUnfree = true;
          config.nvidia.acceptLicense = true;
        };
        pre-commit-check = pre-commit-hooks.lib.${system}.run {
          src = ./.;
          configPath = ".pre-commit-config-nix.yaml";
          hooks = {
            eclint.enable = true;
            end-of-file-fixer.enable = true;
            flake-checker.enable = true;
            nil.enable = true;
            ormolu.enable = true;
            trim-trailing-whitespace.enable = true;
          };
        };
      in
    {
      nixosConfigurations = {
        andromeda = lib.nixosSystem {
          inherit pkgs system;
          modules = [ ./system/andromeda/configuration.nix ];
          specialArgs = { inherit pkgs-unstable; };
        };
        starfish = lib.nixosSystem {
          inherit pkgs system;
          modules = [ ./system/starfish/configuration.nix ];
          specialArgs = { inherit pkgs-unstable; };
        };
      };
      homeConfigurations = {
        william = home-manager.lib.homeManagerConfiguration {
          inherit pkgs;
          modules = [ ./home ];
          extraSpecialArgs = {
            inherit pkgs-unstable;
            inherit sniffy;
          };
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
