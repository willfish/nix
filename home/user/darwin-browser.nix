# Hash-pinned browser runtime from the existing nixpkgs/Playwright revision.
{ pkgs }:
let
  arch = if pkgs.stdenv.hostPlatform.isAarch64 then "arm64" else "x64";
  shell = pkgs.playwright-driver.components.chromium-headless-shell;
in
{
  executable = "${shell}/chrome-headless-shell-mac-${arch}/chrome-headless-shell";
  browsers = pkgs.playwright-driver.selectBrowsers {
    withChromium = false;
    withFirefox = false;
    withWebkit = false;
    withFfmpeg = false;
    withChromiumHeadlessShell = true;
  };
}
