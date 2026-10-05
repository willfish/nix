{ pkgs }:
# Retain the third-party DDGS metasearch engine and the adapter's timeout.
# Its CLI has no timeout option, so configure the dedicated package default.
pkgs.python3Packages.ddgs.overrideAttrs (previous: {
  postPatch = (previous.postPatch or "") + ''
    substituteInPlace ddgs/ddgs.py \
      --replace-fail 'timeout: int | None = 5,' 'timeout: int | None = 15,'
  '';
})
