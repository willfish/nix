# Native and mobile interfaces

Apply `~/.agents/guides/documentation-relevance.md` to prose.
Choose the target OS, input methods and actual toolkit from the brief/repository.
A phone-shaped browser preview is not a verified native app. Source changes and
round-trip checks here apply only to implementation; advice/critique report
findings and missing evidence without changing reviewed artifacts.

## Preserve platform behaviour

| Target | Default authority and implementation direction |
| --- | --- |
| Apple | [Human Interface Guidelines](https://developer.apple.com/design/human-interface-guidelines/), existing SwiftUI/UIKit/AppKit controls and accessibility semantics |
| Android | [Adaptive apps](https://developer.android.com/develop/ui/compose/build-adaptive-apps), existing Compose/Views and Material conventions |
| Linux desktop | Existing GTK/libadwaita or Qt toolkit; [GNOME HIG](https://developer.gnome.org/hig/) when using GTK |
| Windows | Existing WinUI/toolkit and [Windows design guidance](https://learn.microsoft.com/en-us/windows/apps/design/) |
| React Native/Flutter | Existing navigation, semantics and platform components; preserve native back, keyboard and accessibility behaviour rather than drawing a website |
| Mobile web/PWA | Explicitly identify it as web-based; use `references/interfaces.md` plus offline, installability and mobile lifecycle checks below |

Do not migrate frameworks for a visual change. In greenfield work, choose the
platform's maintained native controls unless a real shared-code requirement
justifies another toolkit. Shared semantic tokens can map to different physical
sizes, typography and controls on each platform. Match platform units, not raw
CSS pixels: Apple touch guidance and Android's 48dp targets are different systems.

## Desktop

Design resizable windows, minimum useful size, keyboard shortcuts, menus, focus
order, tab traversal and escape/cancel. Respect window manager conventions,
clipboard and file dialogs. Distinguish window close, background operation and
quit. Keep asynchronous feedback and recovery visible without unnecessary modals.
Expose labels, roles, values and state through the toolkit accessibility API.
Test high-DPI scaling, light/dark/high-contrast settings where supported and large
text. A custom-painted control needs an explicit semantics/keyboard solution.

## Mobile

Define navigation and platform back behaviour, safe areas, reachability and
orientation/window-size adaptation. Account for the software keyboard, input
method composition, obscured fields and persistent actions. Do not disable text
scaling or make drag/swipe the only path to an operation.

Design permission denial/revocation, offline and partial data, app suspension,
resume, interrupted submission, duplicate prevention and recovery of drafts.
Do not request permissions merely to simplify a demo. Avoid leaking sensitive
content into notifications or app-switcher previews. Use system settings and
platform pickers instead of rebuilding permission or file-selection UI.

For PWAs, distinguish browser navigation from in-app state, state supported
install/offline behaviour and test service-worker updates and stale data. A
responsive page alone does not prove installability or native platform behaviour.

## Evidence and limits

Implement editable source and build using the repository's authorized toolchain.
Run one successful task and an interruption/error recovery path in the actual
runtime. Inspect the platform accessibility tree, focus/back/keyboard path and
large text, then capture and read final rendered images. Test on a simulator or
device for the intended mobile OS; desktop resizing cannot substitute for it.

If the SDK, licensed tools, device or host is unavailable, continue safe design
and source work, then report the exact unexecuted build/runtime/AT checks. Label
prototypes and previews by their actual runtime. Do not claim iOS verification
from Linux, Android verification from a browser, or accessibility from a screenshot.
Request access or licence approval through the coordinator when required. Never
accept SDK terms, install on a personal device or change device settings silently.

When assessing harness breadth, a Linux native fixture and mobile PWA can prove
those specific outputs only. Future SwiftUI/Compose requests still require their
own target-specific verification before an implementation completion claim.
