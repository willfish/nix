{

  description = "My personal NixOS configuration";

  inputs = {
    nixpkgs.url = "nixpkgs/nixos-23.11";
    nixpkgs-unstable.url = "nixpkgs/nixos-unstable";

    home-manager = {
      url = "github:nix-community/home-manager/release-23.11";

      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { self, nixpkgs, nixpkgs-unstable, ... }@inputs:
    let
      lib = nixpkgs.lib;
      system = "x86_64-linux";

      pkgs = import nixpkgs {inherit system; config.allowUnfree = true; };
      pkgs-unstable = import nixpkgs-unstable {inherit system; config.allowUnfree = true; };
    in {

    nixosConfigurations = {
      andromeda = lib.nixosSystem {
        inherit pkgs;
        modules = [./system/configuration.nix];
        specialArgs = { inherit pkgs-unstable; };
      };
    };

    homeConfigurations = {
      william = inputs.home-manager.lib.homeManagerConfiguration {
        inherit pkgs;
        modules = [./home];
        extraSpecialArgs = { inherit pkgs-unstable; };
      };
    };
  };
}
