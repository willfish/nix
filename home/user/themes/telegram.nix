# Telegram resolves unspecified palette roles through its native defaults/aliases.
# Keep media, avatar and status colours distinct; replace the main UI surfaces.
{ lib, pkgs }:
p:
let
  render = import ./render.nix { inherit lib; };
  selected = render.selectionPair p;
  active = render.selectionPair (p // { base02 = p.base0D; });
  roles = names: colour: lib.genAttrs names (_: "#${colour}");
  colours =
    roles [
      "windowBg"
      "dialogsBg"
      "historyUnreadBarBg"
      "msgInBg"
      "sideBarBgActive"
    ] p.base00
    // roles [
      "windowBgOver"
      "windowShadowFgFallback"
      "menuBg"
      "tooltipBg"
      "emojiPanCategories"
      "msgOutBg"
      "sideBarBg"
    ] p.base01
    // roles [
      "windowFg"
      "windowBoldFg"
      "windowBoldFgOver"
      "menuSubmenuArrowFg"
      "tooltipFg"
      "titleFgActive"
      "boxTitleFg"
      "dialogsArchiveFg"
      "dialogsArchiveFgOver"
      "msgServiceFg"
    ] p.base05
    // roles [
      "windowSubTextFg"
      "windowSubTextFgOver"
      "menuIconFg"
      "menuIconFgOver"
      "smallCloseIconFg"
      "smallCloseIconFgOver"
      "placeholderFgActive"
      "titleFg"
      "titleButtonFg"
      "titleButtonFgOver"
      "boxTitleAdditionalFg"
      "dialogsSendingIconFg"
      "emojiIconFg"
      "emojiSubIconFgActive"
      "historySendingInIconFg"
      "msgInDateFg"
      "msgOutDateFg"
      "sideBarTextFg"
      "sideBarIconFg"
    ] p.base04
    // roles [
      "inputBorderFg"
      "checkboxFg"
      "tooltipBorderFg"
      "menuFgDisabled"
      "dialogsUnreadBgMuted"
      "sideBarBadgeBgMuted"
      "trayCounterBgMute"
    ] p.base03
    // roles [
      "windowActiveTextFg"
      "activeLineFg"
      "filterInputBorderFg"
      "historyUnreadBarFg"
      "msgOutServiceFg"
      "msgOutReplyBarColor"
      "msgInMonoFg"
      "msgOutMonoFg"
      "msgFileThumbLinkOutFg"
      "msgWaveformOutActive"
      "sideBarTextFgActive"
      "sideBarIconFgActive"
      "mainMenuCloudBg"
      "profileStatusFgOver"
      "currencyFg"
    ] p.base0D
    // roles [
      "windowBgActive"
      "activeButtonBgOver"
      "activeButtonBgRipple"
      "dialogsBgActive"
      "sideBarBadgeBg"
      "msgFileInBgOver"
      "msgFileOutBg"
    ] active.bg
    // roles [
      "windowFgActive"
      "activeButtonSecondaryFg"
      "dialogsDraftFgActive"
      "dialogsSendingIconFgActive"
      "dialogsOnlineBadgeFgActive"
      "sideBarBadgeFg"
    ] active.fg
    // roles [
      "windowBgRipple"
      "lightButtonBgOver"
      "lightButtonBgRipple"
      "attentionButtonBgOver"
      "attentionButtonBgRipple"
      "titleButtonBgOver"
      "msgInBgSelected"
      "msgOutBgSelected"
      "msgServiceBgSelected"
      "sideBarBgRipple"
      "searchedTextMatchBg"
      "searchedTextCurrentMatchBg"
    ] selected.bg
    // roles [
      "historyTextInFgSelected"
      "historyTextOutFgSelected"
      "historyLinkInFgSelected"
      "historyLinkOutFgSelected"
      "msgInServiceFgSelected"
      "msgOutServiceFgSelected"
      "msgInDateFgSelected"
      "msgOutDateFgSelected"
      "msgFileThumbLinkOutFgSelected"
      "historyOutIconFgSelected"
      "searchedTextMatchFg"
      "searchedTextCurrentMatchFg"
    ] selected.fg
    // roles [
      "attentionButtonFg"
      "attentionButtonFgOver"
      "activeLineFgError"
      "boxTextFgError"
      "dialogsDraftFg"
      "titleButtonCloseBgOver"
      "trayCounterBg"
    ] p.base08
    // roles [
      "boxTextFgGood"
      "dialogsSentIconFg"
      "dialogsSentIconFgOver"
      "dialogsOnlineBadgeFg"
      "historyOutIconFg"
      "historySendingOutIconFg"
      "historyCallArrowInFg"
      "paymentsTipActive"
    ] p.base0B
    // {
      menuSeparatorFg = "#${p.base03}60";
      sliderBgInactive = "#${p.base03}";
      emojiPanHeaderBg = "#${p.base00}f2";
      stickerPreviewBg = "#${p.base00}b0";
      msgServiceBg = "#${p.base01}e6";
      toastBg = "#${p.base01}f2";
      toastFg = "#${p.base05}";
      scrollBarBg = "#${p.base04}80";
      scrollBarBgOver = "#${p.base04}bc";
      scrollBg = "#${p.base03}40";
      scrollBgOver = "#${p.base03}60";
      historyScrollBarBg = "#${p.base04}80";
      historyScrollBarBgOver = "#${p.base04}bc";
      historyScrollBg = "#${p.base03}40";
      historyScrollBgOver = "#${p.base03}60";
      msgSelectOverlay = "#${p.base0D}40";
      msgStickerOverlay = "#${p.base0D}60";
    };
in
(import ../../../programs/telegram-theme { inherit pkgs; }) {
  background = p.base00;
  palette =
    lib.concatStringsSep "\n" (lib.mapAttrsToList (name: value: "${name}: ${value};") colours) + "\n";
}
