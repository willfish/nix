import QtQuick
import Quickshell
import Quickshell.Io
import "spotify" as Spotify

// Host adapter for the pinned Omarchy Spotify plugin. Waybar stays the bar.
// This process only owns the player service and the full player window.
ShellRoot {
  id: shell

  property string configPath: Quickshell.env("HYPR_SPOTIFY_SETTINGS")
  property var shellConfig: ({
    version: 1,
    plugins: [{ id: "quickshell.spotify" }]
  })
  readonly property var manifest: ({
    id: "quickshell.spotify",
    __sourceDir: Quickshell.env("HYPR_SPOTIFY_PLUGIN")
  })

  function applyConfig(raw) {
    var text = String(raw || "").trim()
    if (!text) return
    try {
      var parsed = JSON.parse(text)
      if (parsed && parsed.version === 1) shellConfig = parsed
    } catch (e) {
      console.warn("spotify settings parse failed", e)
    }
  }

  function updateEntryInline(moduleName, settings) {
    if (String(moduleName || "") !== "quickshell.spotify") return false
    var next = JSON.parse(JSON.stringify(shellConfig || {}))
    next.version = 1
    var plugins = Array.isArray(next.plugins) ? next.plugins.slice() : []
    var found = false
    for (var i = 0; i < plugins.length; i++) {
      if (!plugins[i] || String(plugins[i].id || "") !== "quickshell.spotify") continue
      var merged = JSON.parse(JSON.stringify(plugins[i]))
      for (var key in settings) merged[key] = settings[key]
      merged.id = "quickshell.spotify"
      plugins[i] = merged
      found = true
    }
    if (!found) {
      var created = JSON.parse(JSON.stringify(settings || {}))
      created.id = "quickshell.spotify"
      plugins.push(created)
    }
    next.plugins = plugins
    shellConfig = next
    configFile.setText(JSON.stringify(next, null, 2) + "\n")
    return true
  }

  function hide(pluginId) {
    if (panel.opened) panel.close()
    return true
  }

  FileView {
    id: configFile
    path: shell.configPath
    watchChanges: true
    atomicWrites: true
    printErrors: false
    onLoaded: shell.applyConfig(text())
    onLoadFailed: shell.applyConfig("")
    onFileChanged: reload()
  }

  Spotify.Service {
    id: service
    shell: shell
    manifest: shell.manifest
  }

  Spotify.Panel {
    id: panel
    shell: shell
    manifest: shell.manifest
    service: service
  }

  IpcHandler {
    target: "spotify"
    function toggle(): string {
      if (panel.opened) {
        panel.requestClose()
        return "closed"
      }
      panel.open("{}")
      return "opened"
    }
    function show(): string {
      panel.open("{}")
      return "opened"
    }
  }
}
