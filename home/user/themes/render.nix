{ lib }:
let
  hex = p: lib.mapAttrs (_: c: "#${c}") p;
  hexDigits = lib.stringToCharacters "0123456789abcdef";
  hexValue = lib.listToAttrs (
    lib.imap0 (index: digit: {
      name = digit;
      value = index;
    }) hexDigits
  );
  fromHex =
    text:
    lib.foldl' (acc: digit: acc * 16 + hexValue.${lib.toLower digit}) 0 (lib.stringToCharacters text);
  toHex2 = value: "${lib.elemAt hexDigits (value / 16)}${lib.elemAt hexDigits (lib.mod value 16)}";
  # 35% toward the target. Integer division is the contract checked by tests.
  mix =
    source: target:
    lib.concatMapStrings
      (
        offset:
        let
          current = fromHex (lib.substring offset 2 source);
          destination = fromHex (lib.substring offset 2 target);
        in
        toHex2 (current + (35 * (destination - current)) / 100)
      )
      [
        0
        2
        4
      ];
  channelSum =
    colour:
    lib.foldl' (acc: offset: acc + fromHex (lib.substring offset 2 colour)) 0 [
      0
      2
      4
    ];
  # Rec. 709 luma, 0-255. A gap of 120 is enough for highlighted text without
  # inventing a colour outside the palette.
  luma =
    colour:
    (
      2126 * fromHex (lib.substring 0 2 colour)
      + 7152 * fromHex (lib.substring 2 2 colour)
      + 722 * fromHex (lib.substring 4 2 colour)
    )
    / 10000;
  gap =
    a: b:
    let
      delta = luma a - luma b;
    in
    if delta < 0 then -delta else delta;
  minHighlightGap = 120;
  selectionPair =
    p:
    let
      bg = p.base02;
      ranked = lib.sort (a: b: gap a bg > gap b bg) [
        p.base00
        p.base05
        p.base07
      ];
      fg = if gap p.base05 bg >= minHighlightGap then p.base05 else lib.head ranked;
      nudged = mix bg (if luma fg > 128 then "000000" else "ffffff");
    in
    {
      inherit fg;
      bg = if gap fg bg >= minHighlightGap then bg else nudged;
    };
in
{
  inherit selectionPair;

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

  # Omarchy's btop template, filled from the shared Base16 roles. blue is not a
  # Base16 slot; pass the upstream blue when it differs from accent.
  btop =
    p:
    let
      c = hex p;
      blue = c.blue or c.base0D;
    in
    ''
      # Generated from the selected palette.
      theme[main_bg]="${c.base00}"
      theme[main_fg]="${c.base05}"
      theme[title]="${c.base05}"
      theme[hi_fg]="${c.base0D}"
      theme[selected_bg]="${c.base02}"
      theme[selected_fg]="${c.base0D}"
      theme[inactive_fg]="${c.base03}"
      theme[graph_text]="${c.base06}"
      theme[meter_bg]="${c.base02}"
      theme[proc_misc]="${c.base06}"
      theme[cpu_box]="${c.base0E}"
      theme[mem_box]="${c.base0B}"
      theme[net_box]="${c.base08}"
      theme[proc_box]="${c.base0D}"
      theme[div_line]="${c.base03}"
      theme[temp_start]="${c.base0B}"
      theme[temp_mid]="${c.base0A}"
      theme[temp_end]="${c.base08}"
      theme[cpu_start]="${c.base0C}"
      theme[cpu_mid]="${blue}"
      theme[cpu_end]="${c.base0E}"
      theme[free_start]="${c.base0E}"
      theme[free_mid]="${blue}"
      theme[free_end]="${c.base0C}"
      theme[cached_start]="${blue}"
      theme[cached_mid]="${c.base0C}"
      theme[cached_end]="${c.base0E}"
      theme[available_start]="${c.base0A}"
      theme[available_mid]="${c.base08}"
      theme[available_end]="${c.base08}"
      theme[used_start]="${c.base0B}"
      theme[used_mid]="${c.base0C}"
      theme[used_end]="${blue}"
      theme[download_start]="${c.base0A}"
      theme[download_mid]="${c.base08}"
      theme[download_end]="${c.base08}"
      theme[upload_start]="${c.base0B}"
      theme[upload_mid]="${c.base0C}"
      theme[upload_end]="${blue}"
      theme[process_start]="${c.base0C}"
      theme[process_mid]="${blue}"
      theme[process_end]="${c.base0E}"
      theme[gradient_color_0]="${c.base00}"
      theme[gradient_color_1]="${c.base01}"
      theme[gradient_color_2]="${c.base02}"
      theme[gradient_color_3]="${c.base03}"
      theme[gradient_color_4]="${c.base04}"
      theme[gradient_color_5]="${c.base05}"
      theme[gradient_color_6]="${c.base06}"
      theme[gradient_color_7]="${c.base07}"
    '';

  ghostty =
    p:
    let
      c = hex p;
      # The renderer sees one palette, not a mode flag. Light paper is lighter
      # than its text; dark paper is darker than its text.
      brightTarget = if channelSum p.base00 > channelSum p.base05 then p.base07 else p.base05;
      bright = colour: "#${mix colour brightTarget}";
    in
    ''
      background = ${c.base00}
      foreground = ${c.base05}
      cursor-color = ${c.base0D}
      cursor-text = ${c.base00}
      selection-background = #${(selectionPair p).bg}
      selection-foreground = #${(selectionPair p).fg}
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
          (bright p.base08)
          (bright p.base0B)
          (bright p.base0A)
          (bright p.base0D)
          (bright p.base0E)
          (bright p.base0C)
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
        thinkingXhigh = c.base08;
        thinkingMax = c.base09;
        bashMode = c.base0A;
      };
      export = {
        pageBg = c.base00;
        cardBg = c.base01;
        infoBg = c.base02;
      };
    };
}
