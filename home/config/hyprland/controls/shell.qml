import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Ui
import "plugins/panels/audio" as Audio
import "plugins/panels/bluetooth" as Bluetooth
import "plugins/panels/network" as Network
import "plugins/panels/tailscale" as Tailscale
import "plugins/polkit" as Polkit

// Only a host adapter. The panel implementations and shared UI are upstream.
ShellRoot {
  id: host
  property var options: JSON.parse(Quickshell.env("HYPR_CONTROLS_SETTINGS"))
  property var panels: ({ audio: audio, bluetooth: bluetooth, network: network, tailscale: tailscale })

  function firstPartyServiceFor(id) { return null }
  function summon(id, payload) {
    // The optional volume OSD is not a second desktop service here.
  }
  function switchPanel(owner, direction) {
    var list = [audio, bluetooth, network, tailscale]
    var index = list.indexOf(owner)
    if (index < 0) return false
    list[(index + direction + list.length) % list.length].open()
    return true
  }

  PluginBarApi {
    id: barApi
    pluginId: "hyprland-controls"
    moduleName: "hyprland-controls"
    shell: host
    position: host.options.position
    vertical: position === "left" || position === "right"
    barSize: host.options.width
    foreground: Color.popups.text
    barForeground: Color.bar.text
    background: Color.popups.background
    urgent: Color.urgent
    fontFamily: host.options.font
    property var externalScreen: Quickshell.screens[0] ?? null
    property point externalAnchor: Qt.point(0, 0)
    _requestPopout: function(owner) {
      var previous = barApi.activePopout
      barApi.activePopout = owner
      if (previous && previous !== owner) previous.closeForPopoutSwitch()
    }
    _releasePopout: function(owner) {
      if (barApi.activePopout === owner) barApi.activePopout = null
    }
    _switchPanelFrom: function(owner, direction) {
      return host.switchPanel(owner, direction)
    }
    _moduleWidgets: function(id) {
      return Object.values(host.panels).filter(panel => panel.moduleName === id)
    }
    _run: function(command) { Quickshell.execDetached(["bash", "-c", command]) }
  }

  // Unmapped items: their original bar buttons create no visible second bar.
  // KeyboardPanel gets its output/anchor from the small portability patch.
  Item {
    Audio.Panel { id: audio; bar: barApi }
    Bluetooth.Panel { id: bluetooth; bar: barApi }
    Network.Panel { id: network; bar: barApi }
    Tailscale.Panel { id: tailscale; bar: barApi }
    // Same palette as the panels. Replaces the unthemed Hyprland agent.
    Polkit.PolkitAgent { }
  }

  IpcHandler {
    target: "controls"
    function toggle(name: string, output: string, x: real, y: real): void {
      var panel = host.panels[name]
      if (!panel) return
      if (panel.opened) { panel.close(); return }
      barApi.externalScreen = Quickshell.screens.find(s => s.name === output)
        ?? Quickshell.screens[0]
      barApi.externalAnchor = Qt.point(x, y)
      panel.open()
    }
  }
}
