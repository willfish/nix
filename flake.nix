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
    nixos-hardware.url = "github:NixOS/nixos-hardware/master";
  };
  outputs =
    {
      nixpkgs-unstable,
      pre-commit-hooks,
      home-manager,
      sniffy,
      smailer,
      nixos-hardware,
      ...
    }:
    let
      system = "x86_64-linux";
      lib = nixpkgs-unstable.lib;
      pkgs = import nixpkgs-unstable {
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
          inherit pkgs system;
          modules = [ ./system/andromeda/configuration.nix ];
          specialArgs = { inherit pkgs-unstable; };
        };
        starfish = lib.nixosSystem {
          inherit pkgs system;
          modules = [ ./system/starfish/configuration.nix ];
          specialArgs = { inherit pkgs-unstable; };
        };
        foundation = lib.nixosSystem {
          inherit pkgs system;
          modules = [ ./system/foundation/configuration.nix ];
          specialArgs = {
            inherit pkgs-unstable;
            inherit nixos-hardware;
          };
        };
      };
      homeConfigurations = {
        william = home-manager.lib.homeManagerConfiguration {
          inherit pkgs;
          modules = [ ./home ];
          extraSpecialArgs = {
            inherit pkgs-unstable;
            inherit sniffy smailer;
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
