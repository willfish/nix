{
  description = ''
    Andromeda and Starfish NixOS configurations

    Andromeda is a NixOS configuration for my Thelio Major Threadripper desktop computer.
    Starfish is a NixOS configuration for my Dell Precision 5750 laptop.

    Home Manager configuration for my user account on all systems.
  '';

  inputs = {
    nixpkgs.url = "nixpkgs/nixos-24.05";
    nixpkgs-unstable.url = "nixpkgs/nixos-unstable";

    home-manager = {
      url = "github:nix-community/home-manager/release-24.05";

      inputs.nixpkgs.follows = "nixpkgs";
    };

    firefox-addons = {
      url = "gitlab:rycee/nur-expressions?dir=pkgs/firefox-addons";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { nixpkgs, nixpkgs-unstable, ... }@inputs:
    let
      lib = nixpkgs.lib;
      system = builtins.currentSystem;

      pkgs = import nixpkgs { inherit system; config.allowUnfree = true; config.nvidia.acceptLicense = true; };
      pkgs-unstable = import nixpkgs-unstable { inherit system; config.allowUnfree = true; config.nvidia.acceptLicense = true; };

    in
    {

      nixosConfigurations = {
        andromeda = lib.nixosSystem {
          inherit pkgs;
          modules = [ ./system/andromeda/configuration.nix ];
          specialArgs = { inherit pkgs-unstable; };
        };

        starfish = lib.nixosSystem {
          inherit pkgs;
          modules = [ ./system/starfish/configuration.nix ];
          specialArgs = { inherit pkgs-unstable; };
        };
      };

      homeConfigurations = {
        william = inputs.home-manager.lib.homeManagerConfiguration {
          inherit pkgs;
          modules = [ ./home ];
          extraSpecialArgs = {
            inherit pkgs-unstable;
            inherit inputs;
          };
        };
      };
    };
}
