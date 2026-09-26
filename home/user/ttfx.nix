{
  rustPlatform,
  fetchFromGitHub,
  nasm,
  python3,
  binutils,
}:
rustPlatform.buildRustPackage {
  pname = "ttfx";
  version = "0.3.3-asm";

  # DHH's x86-64 assembly engine. Not on master yet; asm-zen5 links it in
  # when NASM is present and the CPU tier matches.
  src = fetchFromGitHub {
    owner = "omacom";
    repo = "ttfx";
    rev = "35c13cc59c563d6c596ec594e8f0fc03bd5b75e0";
    hash = "sha256-I7rA+LMH9CL/fEJTHzUxMh/ECg8SYGv8/wzwxI9FVdE=";
  };

  cargoHash = "sha256-JKfEgISmX8iIw5Bcr0u7pb5J5TsKCvNSQn3E9tH7Wes=";

  nativeBuildInputs = [
    nasm
    python3
    binutils
  ];

  # The NASM objects use PC32 relocations. Nix's Rust target links PIE, which
  # binutils 2.46 rejects. The upstream binary is not PIE either.
  preBuild = ''
    export RUSTFLAGS="''${RUSTFLAGS:-} -C link-arg=-no-pie"
  '';

  meta.mainProgram = "ttfx";
}
