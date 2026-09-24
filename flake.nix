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
    # Data-only theme inputs. omarchy-theme-* inputs are discovered automatically.
    omarchy = {
      url = "github:basecamp/omarchy";
      flake = false;
    };
    # BEGIN generated Omarchy community inputs
    omarchy-theme-aetheria = {
      url = "git+https://github.com/JJDizz1L/aetheria?shallow=1";
      flake = false;
    };
    omarchy-theme-all-hallow-s-eve = {
      url = "git+https://github.com/guilhermetk/omarchy-all-hallows-eve-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-amberbyte = {
      url = "git+https://github.com/tahfizhabib/omarchy-amberbyte-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-arc-blueberry = {
      url = "git+https://github.com/vale-c/omarchy-arc-blueberry?shallow=1";
      flake = false;
    };
    omarchy-theme-archwave = {
      url = "git+https://github.com/davidguttman/archwave?shallow=1";
      flake = false;
    };
    omarchy-theme-artzen = {
      url = "git+https://github.com/tahfizhabib/omarchy-artzen-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-ash = {
      url = "git+https://github.com/bjarneo/omarchy-ash-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-atelier = {
      url = "git+https://github.com/atif-1402/omarchy-atelier-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-aura = {
      url = "git+https://github.com/bjarneo/omarchy-aura-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-ayaka = {
      url = "git+https://github.com/abhijeet-swami/omarchy-ayaka-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-azure-glow = {
      url = "git+https://github.com/Hydradevx/omarchy-azure-glow-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-batman = {
      url = "git+https://github.com/OldJobobo/omarchy-batman-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-batou = {
      url = "git+https://github.com/HANCORE-linux/omarchy-batou-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-bauhaus = {
      url = "git+https://github.com/somerocketeer/omarchy-bauhaus-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-biscuit-de-mar-dark = {
      url = "git+https://github.com/OldJobobo/omarchy-biscuit-de-mar-dark-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-black-arch = {
      url = "git+https://github.com/ankur311sudo/black_arch?shallow=1";
      flake = false;
    };
    omarchy-theme-black-gold = {
      url = "git+https://github.com/HANCORE-linux/omarchy-blackgold-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-black-sand = {
      url = "git+https://github.com/pkovzz/omarchy-black-sand-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-black-turq = {
      url = "git+https://github.com/HANCORE-linux/omarchy-blackturq-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-blue-ridge-dark = {
      url = "git+https://github.com/hipsterusername/omarchy-blueridge-dark-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-bluedotrb = {
      url = "git+https://github.com/dotsilva/omarchy-bluedotrb-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-castle-on-a-lake = {
      url = "git+https://github.com/shmall03/omarchy-castle-on-a-lake-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-catppuccin-mocha-dark = {
      url = "git+https://github.com/Luquatic/omarchy-catppuccin-dark?shallow=1";
      flake = false;
    };
    omarchy-theme-cincinnati = {
      url = "git+https://github.com/jkwuc89/omarchy-cincinnati-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-citrus-cynapse = {
      url = "git+https://github.com/Grey-007/citrus-cynapse?shallow=1";
      flake = false;
    };
    omarchy-theme-city-783 = {
      url = "git+https://github.com/OldJobobo/omarchy-city-783-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-cobalt2 = {
      url = "git+https://github.com/hoblin/omarchy-cobalt2-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-coffee = {
      url = "git+https://github.com/megabyte0x/omarchy-coffee-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-coffee-latte = {
      url = "git+https://github.com/megabyte0x/omarchy-coffee-latte-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-commit = {
      url = "git+https://github.com/c0ze/omarchy-commit-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-cpunk = {
      url = "git+https://github.com/stannorbvb-cmd/cpunk?shallow=1";
      flake = false;
    };
    omarchy-theme-crimson-gold = {
      url = "git+https://github.com/knappkevin/omarchy-crimson-gold-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-darcula = {
      url = "git+https://github.com/noahljungberg/omarchy-darcula-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-demon = {
      url = "git+https://github.com/HANCORE-linux/omarchy-demon-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-dos-moos = {
      url = "git+https://github.com/HANCORE-linux/omarchy-dos-moos-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-dotrb = {
      url = "git+https://github.com/dotsilva/omarchy-dotrb-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-drac = {
      url = "git+https://github.com/ShehabShaef/omarchy-drac-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-dracula = {
      url = "git+https://github.com/catlee/omarchy-dracula-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-eldritch = {
      url = "git+https://github.com/eldritch-theme/omarchy?shallow=1";
      flake = false;
    };
    omarchy-theme-event-horizon = {
      url = "git+https://github.com/OldJobobo/omarchy-event-horizon-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-evergarden = {
      url = "git+https://github.com/celsobenedetti/omarchy-evergarden?shallow=1";
      flake = false;
    };
    omarchy-theme-felix = {
      url = "git+https://github.com/TyRichards/omarchy-felix-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-fireside = {
      url = "git+https://github.com/bjarneo/omarchy-fireside-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-flat-dracula = {
      url = "git+https://github.com/OldJobobo/omarchy-flat-dracula-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-flexoki-dark = {
      url = "git+https://github.com/euandeas/omarchy-flexoki-dark-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-forest-green = {
      url = "git+https://github.com/abhijeet-swami/omarchy-forest-green-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-frost = {
      url = "git+https://github.com/bjarneo/omarchy-frost-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-fuchsblau = {
      url = "git+https://github.com/fuchsblau/omarchy-fuchsblau-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-futurism = {
      url = "git+https://github.com/bjarneo/omarchy-futurism-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-futurist = {
      url = "git+https://github.com/benwillems/omarchy-futurist-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-gand = {
      url = "git+https://github.com/c0ze/omarchy-gand-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-ghost-pastel = {
      url = "git+https://github.com/row-huh/omarchy-ghost-pastel-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-gold-rush = {
      url = "git+https://github.com/tahayvr/omarchy-gold-rush-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-golden-brown = {
      url = "git+https://github.com/atif-1402/omarchy-golden-brown-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-greek-noir = {
      url = "git+https://github.com/HANCORE-linux/omarchy-greek-noir-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-green-garden = {
      url = "git+https://github.com/kalk-ak/omarchy-green-garden-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-gruvbox-material = {
      url = "git+https://github.com/curbol/omarchy-gruvbox-material?shallow=1";
      flake = false;
    };
    omarchy-theme-harbor = {
      url = "git+https://github.com/HANCORE-linux/omarchy-harbor-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-harbor-dark = {
      url = "git+https://github.com/HANCORE-linux/omarchy-harbordark-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-hermarchy = {
      url = "git+https://github.com/archer-clawbot/omarchy-hermarchy-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-hinterlands = {
      url = "git+https://github.com/OldJobobo/omarchy-hinterlands-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-infernium = {
      url = "git+https://github.com/RiO7MAKK3R/omarchy-infernium-dark-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-inky-pinky = {
      url = "git+https://github.com/HANCORE-linux/omarchy-inkypinky-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-japan-night = {
      url = "git+https://github.com/devgtv/omarchy-japan-night-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-lamplight = {
      url = "git+https://github.com/thisisgm/omarchy-lamplight-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-lawson-night = {
      url = "git+https://github.com/phuclh/omarchy-lawson-night-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-map-quest = {
      url = "git+https://github.com/ItsABigIgloo/omarchy-mapquest-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-mars = {
      url = "git+https://github.com/steve-lohmeyer/omarchy-mars-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-matrix = {
      url = "git+https://github.com/BVisagie/omarchy-matrix-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-mechanoonna = {
      url = "git+https://github.com/HANCORE-linux/omarchy-mechanoonna-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-midnight = {
      url = "git+https://github.com/JaxonWright/omarchy-midnight-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-milky-matcha = {
      url = "git+https://github.com/hipsterusername/omarchy-milkmatcha-light-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-mini-jcw = {
      url = "git+https://github.com/davydotcom/omarchy-mini-jcw-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-monochrome = {
      url = "git+https://github.com/Swarnim114/omarchy-monochrome-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-monokai = {
      url = "git+https://github.com/bjarneo/omarchy-monokai-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-moodpeak = {
      url = "git+https://github.com/HANCORE-linux/omarchy-moodpeak-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-nagai-poolside = {
      url = "git+https://github.com/somerocketeer/omarchy-nagai-poolside-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-naysayer = {
      url = "git+https://github.com/brianblakely/omarchy-naysayer-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-neo-sploosh = {
      url = "git+https://github.com/monoooki/omarchy-neo-sploosh-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-neon-dusk = {
      url = "git+https://github.com/daniel-felipe/omarchy-neon-dusk-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-neovoid = {
      url = "git+https://github.com/RiO7MAKK3R/omarchy-neovoid-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-neptune-blue = {
      url = "git+https://github.com/davydotcom/omarchy-neptune-blue-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-nes = {
      url = "git+https://github.com/bjarneo/omarchy-nes-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-noir = {
      url = "git+https://github.com/tahadx/omarchy-noir-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-nujabes = {
      url = "git+https://github.com/HalmyLyseas/omarchy-nujabes-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-oligarchy = {
      url = "git+https://github.com/EF-Code/omarchy-oligarchy-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-omacarchy = {
      url = "git+https://github.com/RiO7MAKK3R/omarchy-omacarchy-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-omaled = {
      url = "git+https://github.com/brianblakely/omarchy-omaled-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-one-dark = {
      url = "git+https://github.com/joaopinto15/omarchy-one-dark-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-one-dark-pro = {
      url = "git+https://github.com/sc0ttman/omarchy-one-dark-pro-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-oxo-carbon = {
      url = "git+https://github.com/HANCORE-linux/omarchy-oxocarbon-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-pagan = {
      url = "git+https://github.com/c0ze/omarchy-pagan-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-pandora = {
      url = "git+https://github.com/imbypass/omarchy-pandora-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-periphery = {
      url = "git+https://github.com/r-bart/omarchy-periphery-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-pina = {
      url = "git+https://github.com/bjarneo/omarchy-pina-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-pink-blood = {
      url = "git+https://github.com/ITSZXY/pink-blood-omarchy-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-pulsar = {
      url = "git+https://github.com/bjarneo/omarchy-pulsar-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-purple-moon = {
      url = "git+https://github.com/Grey-007/purple-moon?shallow=1";
      flake = false;
    };
    omarchy-theme-purplewave = {
      url = "git+https://github.com/dotsilva/omarchy-purplewave-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-quattrocento-light = {
      url = "git+https://github.com/r-bart/omarchy-quattrocento-light-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-rainy-night = {
      url = "git+https://github.com/atif-1402/omarchy-rainynight-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-red-monarch = {
      url = "git+https://github.com/kamatealif/omarchy-red-monarch-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-red-pill = {
      url = "git+https://github.com/ferlemes/omarchy-red-pill-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-retropc = {
      url = "git+https://github.com/rondilley/omarchy-retropc-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-ristretto-light = {
      url = "git+https://github.com/brokkoli71/omarchy-ristretto-light-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-robzee84 = {
      url = "git+https://github.com/robzolkos/omarchy-robzee84-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-rose-of-dune = {
      url = "git+https://github.com/HANCORE-linux/omarchy-roseofdune-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-rose-pine-dark = {
      url = "git+https://github.com/guilhermetk/omarchy-rose-pine-dark?shallow=1";
      flake = false;
    };
    omarchy-theme-rose-pine-moon = {
      url = "git+https://github.com/Memnoc/omarchy-rose-pine-moon-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-ryu = {
      url = "git+https://github.com/HANCORE-linux/omarchy-ryu-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-saga = {
      url = "git+https://github.com/HANCORE-linux/omarchy-saga-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-sakura = {
      url = "git+https://github.com/bjarneo/omarchy-sakura-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-sakura-mochi = {
      url = "git+https://github.com/OldJobobo/omarchy-sakura-mochi-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-sapphire = {
      url = "git+https://github.com/HANCORE-linux/omarchy-sapphire-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-shades-of-jade = {
      url = "git+https://github.com/HANCORE-linux/omarchy-shadesofjade-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-snow = {
      url = "git+https://github.com/bjarneo/omarchy-snow-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-snow-black = {
      url = "git+https://github.com/ankur311sudo/snow_black?shallow=1";
      flake = false;
    };
    omarchy-theme-solarized = {
      url = "git+https://github.com/Gazler/omarchy-solarized-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-solarized-light = {
      url = "git+https://github.com/dfrico/omarchy-solarized-light-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-solarized-osaka = {
      url = "git+https://github.com/motorsss/omarchy-solarizedosaka-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-space-monkey = {
      url = "git+https://github.com/TyRichards/omarchy-space-monkey-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-starry-night = {
      url = "git+https://github.com/juangalt/omarchy-starry-night-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-starsend = {
      url = "git+https://github.com/r-bart/omarchy-starsend-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-sunset = {
      url = "git+https://github.com/rondilley/omarchy-sunset-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-sunset-drive = {
      url = "git+https://github.com/tahayvr/omarchy-sunset-drive-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-super-game-bro = {
      url = "git+https://github.com/TyRichards/omarchy-super-game-bro-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-synthwave-84 = {
      url = "git+https://github.com/omacom-io/omarchy-synthwave84-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-temerald = {
      url = "git+https://github.com/Ahmad-Mtr/omarchy-temerald-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-terminus = {
      url = "git+https://github.com/r-bart/omarchy-terminus-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-the-greek = {
      url = "git+https://github.com/HANCORE-linux/omarchy-thegreek-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-tokyo-night-oled = {
      url = "git+https://github.com/Justin-De-Sio/omarchy-tokyoled-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-tycho = {
      url = "git+https://github.com/leonardobetti/omarchy-tycho?shallow=1";
      flake = false;
    };
    omarchy-theme-van-gogh = {
      url = "git+https://github.com/Nirmal314/omarchy-van-gogh-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-vault = {
      url = "git+https://github.com/r-bart/omarchy-vault-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-velvet-night = {
      url = "git+https://github.com/HANCORE-linux/omarchy-velvetnight-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-venice-from-above = {
      url = "git+https://github.com/mattbbia/venice-from-above-omarchy?shallow=1";
      flake = false;
    };
    omarchy-theme-vesper = {
      url = "git+https://github.com/thmoee/omarchy-vesper-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-vhs-80 = {
      url = "git+https://github.com/tahayvr/omarchy-vhs80-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-void = {
      url = "git+https://github.com/vyrx-dev/omarchy-void-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-vulkanite = {
      url = "git+https://github.com/kyerpotts/omarchy-vulkanite-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-waffle-cat = {
      url = "git+https://github.com/OldJobobo/omarchy-waffle-cat-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-waveform-dark = {
      url = "git+https://github.com/hipsterusername/omarchy-waveform-dark-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-white-gold = {
      url = "git+https://github.com/HANCORE-linux/omarchy-whitegold-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-windows-dark-mode = {
      url = "git+https://github.com/oldjobobo/omarchy-windows-dark-mode-theme?shallow=1";
      flake = false;
    };
    omarchy-theme-winslow = {
      url = "git+https://github.com/chipkoziara/omarchy-winslow-theme?shallow=1";
      flake = false;
    };
    # END generated Omarchy community inputs
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

          packages = {
            mcp-dap-server = pkgs.callPackage ./home/user/mcp-packages/mcp-dap-server.nix { };
          }
          //
            lib.mapAttrs' (name: package: lib.nameValuePair "theme-${name}" package)
              (import ./home/user/themes/omarchy.nix { inherit lib pkgs; }).packages;

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
              assert !(server.home.sessionVariables ? BROWSER);
              assert !(server.home.sessionVariables ? TERMINAL);
              assert !(lib.any (p: lib.getName p == "gimp") server.home.packages);
              assert lib.any (p: lib.getName p == "gimp") desktop.home.packages;
              assert server.home.file ? ".config/herdr/config.toml";
              assert desktop.programs.brave.enable && desktop.programs.google-chrome.enable;
              assert desktop.systemd.user.services ? nm-auto-secret-agent;
              assert desktop.home.sessionVariables.BROWSER == "brave";
              assert hasNetcat desktop && hasNetcat server && !hasNetcat darwin;
              pkgs.runCommand "home-profile-boundaries" { } "touch $out";
          }
          // lib.optionalAttrs (system == linuxSystem) {
            sddm = import ./tests/sddm.nix { inherit pkgs; };
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
