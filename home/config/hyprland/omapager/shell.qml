import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons
import "omapager" as Omapager

// Host adapter for the pinned Omapager plugin. Waybar stays the bar.
// This process owns the notification bus, the decks and the history panel.
ShellRoot {
  id: host

  property var options: JSON.parse(Quickshell.env("HYPR_OMAPAGER_BAR") || "{}")
  property string configPath: Quickshell.env("HYPR_OMAPAGER_SETTINGS")
  property var widgetSettings: ({})
  property var bar: barApi

  function applyConfig(raw) {
    var text = String(raw || "").trim()
    if (!text) return
    try {
      var parsed = JSON.parse(text)
      if (parsed && typeof parsed === "object") {
        widgetSettings = parsed
        pager.settings = parsed
      }
    } catch (e) {
      console.warn("omapager settings parse failed", e)
    }
  }

  function serviceFor(id) {
    return String(id || "") === "njpatel.omapager" ? daemon : null
  }

  function updateEntryInline(moduleName, settings) {
    if (String(moduleName || "") !== "njpatel.omapager") return false
    var next = JSON.parse(JSON.stringify(settings || {}))
    delete next.id
    widgetSettings = next
    configFile.setText(JSON.stringify(next, null, 2) + "\n")
    return true
  }

  QtObject {
    id: barApi
    property var shell: host
    property string position: host.options.position || "top"
    property int barSize: Number(host.options.width) > 0 ? Number(host.options.width) : 48
    property bool vertical: position === "left" || position === "right"
    property bool barHidden: false
    property color foreground: Color.popups.text
    property color barForeground: Color.bar.text
    property string fontFamily: host.options.font || Style.font.family
    property var externalScreen: Quickshell.screens[0] ?? null
    property point externalAnchor: Qt.point(0, 0)
    function moduleWidgets(id) { return [pager] }
  }

  FileView {
    id: configFile
    path: host.configPath
    watchChanges: true
    atomicWrites: true
    printErrors: false
    onLoaded: host.applyConfig(text())
    onLoadFailed: host.applyConfig("")
    onFileChanged: reload()
  }

  Omapager.Service {
    id: daemon
    shell: host
  }

  Omapager.Widget {
    id: pager
    bar: barApi
  }

  IpcHandler {
    target: "omapager-host"
    function toggle(output: string, x: real, y: real): string {
      var screens = Quickshell.screens || []
      var screen = null
      for (var i = 0; i < screens.length; i++) {
        if (screens[i] && screens[i].name === output) screen = screens[i]
      }
      barApi.externalScreen = screen || screens[0] || null
      barApi.externalAnchor = Qt.point(x, y)
      pager.togglePanel()
      return pager.opened ? "open" : "closed"
    }
  }
}
