{ lib }:
let
  hex = p: lib.mapAttrs (_: c: "#${c}") p;
in
{
  herdr =
    p:
    let
      c = hex p;
    in
    {
      accent = c.base0D;
      panel_bg = c.base00;
      sidebar_bg = c.base01;
      active_row_bg = c.base02;
      selection_bg = c.base02;
      surface0 = c.base01;
      surface1 = c.base02;
      surface_dim = c.base00;
      overlay0 = c.base03;
      overlay1 = c.base04;
      text = c.base05;
      subtext0 = c.base04;
      mauve = c.base0E;
      green = c.base0B;
      yellow = c.base0A;
      red = c.base08;
      blue = c.base0D;
      teal = c.base0C;
      peach = c.base09;
    };

  ghostty =
    p:
    let
      c = hex p;
    in
    ''
      background = ${c.base00}
      foreground = ${c.base05}
      cursor-color = ${c.base0D}
      cursor-text = ${c.base00}
      selection-background = ${c.base02}
      selection-foreground = ${c.base05}
      ${lib.concatStringsSep "\n" (
        lib.imap0 (i: colour: "palette = ${toString i}=${colour}") [
          c.base00
          c.base08
          c.base0B
          c.base0A
          c.base0D
          c.base0E
          c.base0C
          c.base05
          c.base04
          c.base08
          c.base0B
          c.base0A
          c.base0D
          c.base0E
          c.base0C
          c.base06
        ]
      )}
    '';

  pi =
    name: p:
    let
      c = hex p;
    in
    {
      inherit name;
      colors = {
        accent = c.base0D;
        border = c.base03;
        borderAccent = c.base0D;
        borderMuted = c.base03;
        success = c.base0B;
        error = c.base08;
        warning = c.base0A;
        muted = c.base04;
        dim = c.base04;
        text = c.base05;
        thinkingText = c.base04;
        selectedBg = c.base02;
        scrollbarTrack = c.base03;
        scrollbarThumb = c.base04;
        searchMatchBg = c.base02;
        searchMatchText = c.base05;
        userMessageBg = c.base01;
        userMessageText = c.base05;
        customMessageBg = c.base01;
        customMessageText = c.base05;
        customMessageLabel = c.base0D;
        # Successful tools stay neutral; colour indicates exceptions, not every read.
        toolPendingBg = c.base01;
        toolSuccessBg = c.base01;
        toolErrorBg = c.base02;
        toolTitle = c.base0D;
        toolOutput = c.base05;
        mdHeading = c.base0D;
        mdLink = c.base0D;
        mdLinkUrl = c.base04;
        mdCode = c.base0C;
        mdCodeBlock = c.base05;
        mdCodeBlockBorder = c.base03;
        mdQuote = c.base04;
        mdQuoteBorder = c.base03;
        mdHr = c.base03;
        mdListBullet = c.base0D;
        toolDiffAdded = c.base0B;
        toolDiffRemoved = c.base08;
        toolDiffContext = c.base04;
        syntaxComment = c.base04;
        syntaxKeyword = c.base0E;
        syntaxFunction = c.base0D;
        syntaxVariable = c.base05;
        syntaxString = c.base0B;
        syntaxNumber = c.base09;
        syntaxType = c.base0A;
        syntaxOperator = c.base0C;
        syntaxPunctuation = c.base04;
        thinkingOff = c.base03;
        thinkingMinimal = c.base04;
        thinkingLow = c.base0C;
        thinkingMedium = c.base0D;
        thinkingHigh = c.base0E;
        thinkingXhigh = c.base0E;
        thinkingMax = c.base0E;
        bashMode = c.base0A;
      };
      export = {
        pageBg = c.base00;
        cardBg = c.base01;
        infoBg = c.base02;
      };
    };
}
