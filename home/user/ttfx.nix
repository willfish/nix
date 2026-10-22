{
  lib,
  rustPlatform,
  nasm,
  python3,
  binutils,
  source,
}:
rustPlatform.buildRustPackage {
  pname = "ttfx";
  version =
    let
      cargoVersion = builtins.head (
        builtins.match "version = \"([0-9.]+)\"" (
          lib.findFirst (line: builtins.match "version = \"[0-9.]+\"" line != null) null (
            lib.splitString "\n" (builtins.readFile "${source}/Cargo.toml")
          )
        )
      );
    in
    "${cargoVersion}-unstable-${source.shortRev}";

  # DHH's x86-64 assembly engine; asm-zen5 links it in when NASM is present
  # and the CPU tier matches. flake.lock owns the revision.
  src = source;

  cargoHash = "sha256-ntoj5bmAa9U2+3K1UX6HL0t6MjfCYQNe5LuiuNJ/CnY=";

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
