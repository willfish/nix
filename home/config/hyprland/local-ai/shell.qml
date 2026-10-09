import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons
import "localai" as LocalAi

// Waybar remains the bar. Reuse the plugin's popup and full-screen views.
ShellRoot {
  id: host
  property var options: JSON.parse(Quickshell.env("HYPR_LOCAL_AI_BAR") || "{}")

  QtObject {
    id: barApi
    property string position: host.options.position || "left"
    property int barSize: host.options.width || 48
    property bool vertical: position === "left" || position === "right"
    property color foreground: Color.popups.text
    property color barForeground: Color.bar.text
    property color urgent: Color.urgent
    property bool foregroundAnimationEnabled: true
    property string fontFamily: Style.font.family
    property var externalScreen: Quickshell.screens[0] ?? null
    property point externalAnchor: Qt.point(0, 0)
    property var activePopout: null
    function requestPopout(owner) { activePopout = owner }
    function releasePopout(owner) { if (activePopout === owner) activePopout = null }
  }

  LocalAi.Panel { id: ai; bar: barApi }

  IpcHandler {
    target: "local-ai-host"
    function toggle(output: string, x: real, y: real): string {
      var screens = Quickshell.screens || []
      barApi.externalScreen = screens.find(s => s.name === output) || screens[0] || null
      barApi.externalAnchor = Qt.point(x, y)
      if (ai.full) ai.full = false
      else ai.toggle()
      return ai.opened ? "open" : "closed"
    }
    function status(): string { return ai.view.mark || "idle" }
  }
}
