{
  rustPlatform,
  fetchFromGitHub,
}:
rustPlatform.buildRustPackage {
  pname = "ttfx";
  version = "0.3.3";

  src = fetchFromGitHub {
    owner = "omacom";
    repo = "ttfx";
    rev = "54d21f046f22512b113056a1964077d7b7bf04cc";
    hash = "sha256-N28CYWQ71hfMm2dEcfLFf1qsLXWRbCiGd+ygVzCVbU0=";
  };

  cargoHash = "sha256-JKfEgISmX8iIw5Bcr0u7pb5J5TsKCvNSQn3E9tH7Wes=";

  meta.mainProgram = "ttfx";
}
