#!/usr/bin/env python3
"""Adapt pinned Omarchy shell files for the Waybar-hosted Omapager process."""

import sys
from pathlib import Path


def replace_once(text, old, new, label):
    if old not in text:
        raise SystemExit(f"missing {label}")
    return text.replace(old, new, 1)


def main():
    out = Path(sys.argv[1])
    color = out / "shell/Commons/Color.qml"
    panel = out / "shell/Ui/KeyboardPanel.qml"
    color_text = color.read_text()
    theme_path = (
        'readonly property string currentThemePath: '
        'stateHome + "/omarchy/current/theme"'
    )
    theme_override = (
        'readonly property string currentThemePath: '
        'Quickshell.env("HYPR_CONTROLS_THEME")'
    )
    color_text = replace_once(
        color_text, theme_path, theme_override, "theme path"
    )
    def watched(name):
        return (
            f'path: root.currentThemePath + "/{name}"\n'
            '    watchChanges: false'
        )

    def watching(name):
        return (
            f'path: root.currentThemePath + "/{name}"\n'
            '    watchChanges: true\n'
            '    onFileChanged: reload()'
        )

    color_text = replace_once(
        color_text,
        watched("colors.toml"),
        watching("colors.toml"),
        "colors watch",
    )
    color_text = replace_once(
        color_text, watched("shell.toml"), watching("shell.toml"), "shell watch"
    )
    user_shell = (
        'path: root.home + "/.config/omarchy/shell.toml"\n'
        '    watchChanges: true'
    )
    color_text = replace_once(
        color_text,
        user_shell,
        'path: ""\n    watchChanges: false',
        "user shell override",
    )
    color.write_text(color_text)

    panel_text = panel.read_text()
    panel_text = replace_once(
        panel_text,
        "screen: anchorWindow ? anchorWindow.screen : null",
        "screen: bar.externalScreen",
        "panel screen",
    )
    anchor = (
        "if (!anchorWindow) return Qt.point("
        "bar.externalAnchor.x - anchorW / 2, "
        "bar.externalAnchor.y - anchorH / 2)"
    )
    panel_text = replace_once(
        panel_text,
        "if (!anchorItem || !anchorWindow) return Qt.point(0, 0)",
        anchor,
        "panel anchor",
    )
    old_bar = (
        "readonly property real barW: "
        "anchorWindow ? anchorWindow.width : screenW\n"
        "  readonly property real barH: anchorWindow ? anchorWindow.height : 0"
    )
    new_bar = (
        "readonly property real barW: bar.vertical ? bar.barSize : screenW\n"
        "  readonly property real barH: bar.vertical ? screenH : bar.barSize"
    )
    panel_text = replace_once(panel_text, old_bar, new_bar, "panel bar size")
    old_mask = (
        "mask: Region {\n"
        "    width: root.screenW\n"
        "    height: root.screenH\n"
        "  }"
    )
    panel_text = replace_once(
        panel_text,
        old_mask,
        """mask: Region {
    width: root.screenW
    height: root.screenH
    Region {
      intersection: Intersection.Subtract
      x: root.barPos === "right" ? root.screenW - root.bar.barSize : 0
      y: root.barPos === "bottom" ? root.screenH - root.bar.barSize : 0
      width: root.bar.vertical ? root.bar.barSize : root.screenW
      height: root.bar.vertical ? root.screenH : root.bar.barSize
    }
  }""",
        "panel mask",
    )
    panel.write_text(panel_text)


if __name__ == "__main__":
    main()
