"""Smoke-test a built pill in disposable headless Sway, never the live desktop.

Run with a built pi-voice-osd path and an output directory. Requires sway, grim
and dbus-daemon. Uses its own D-Bus with no activatable desktop services:
  python3 tests/voice_pill_native.py BINARY OUTPUT
"""

import json
import os
import pathlib
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

binary = sys.argv[1]
out = pathlib.Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="voice-pill-native-") as temp:
    root = pathlib.Path(temp)
    runtime = root / "runtime"
    runtime.mkdir(mode=0o700)
    (runtime / "pi-voice").mkdir()
    config = root / "sway.conf"
    config.write_text(
        "output * mode 1280x800\noutput * bg #343c49 solid_color\n"
        "seat seat0 fallback true\n"
    )
    env = dict(
        os.environ,
        XDG_RUNTIME_DIR=str(runtime),
        WLR_BACKENDS="headless",
        WLR_RENDERER="pixman",
        WLR_LIBINPUT_NO_DEVICES="1",
        XDG_STATE_HOME=str(root / "state"),
        XDG_CONFIG_HOME=str(root / "config"),
        XDG_CURRENT_DESKTOP="sway",
        GSK_RENDERER="cairo",
        GTK_A11Y="none",
        GIO_USE_VFS="local",
    )
    # User CSS outranks application CSS. Structural transparency must survive
    # a theme that paints every GTK window.
    gtk_config = root / "config/gtk-4.0"
    gtk_config.mkdir(parents=True)
    (gtk_config / "gtk.css").write_text(
        "window, .background { background-color: #ff00ff; }\n"
    )
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("SWAYSOCK", None)
    env.pop("HYPRLAND_INSTANCE_SIGNATURE", None)
    bus_config = root / "bus.conf"
    bus_address = f"unix:path={runtime}/bus"
    bus_config.write_text(f"""<busconfig><type>session</type>
      <listen>{bus_address}</listen><auth>EXTERNAL</auth>
      <policy context="default"><allow send_destination="*"/>
      <allow receive_sender="*"/><allow own="*"/></policy></busconfig>""")
    env["DBUS_SESSION_BUS_ADDRESS"] = bus_address
    server = socket.socket(socket.AF_UNIX)
    server.bind(str(runtime / "pi-voice/control.sock"))
    server.listen()
    server.settimeout(0.2)
    state = {"ok": True, "phase": "idle"}
    finished = threading.Event()

    def serve():
        while not finished.is_set():
            try:
                client, _ = server.accept()
            except socket.timeout:
                continue
            with client:
                client.settimeout(1)
                try:
                    client.recv(1024)
                    client.sendall(json.dumps(state).encode() + b"\n")
                except (OSError, TimeoutError):
                    pass

    thread = threading.Thread(target=serve)
    thread.start()
    swaylog = open(out / "compositor.log", "w")
    applog = open(out / "app.log", "w")
    app = sway = bus = None
    try:
        bus = subprocess.Popen(
            ["dbus-daemon", "--nofork", "--config-file", str(bus_config)],
            env=env,
        )
        sway = subprocess.Popen(
            ["sway", "--unsupported-gpu", "-c", str(config)],
            env=env,
            stdout=swaylog,
            stderr=swaylog,
        )
        for _ in range(100):
            sockets = list(runtime.glob("wayland-*"))
            sockets = [p for p in sockets if not p.name.endswith(".lock")]
            if sockets:
                break
            if sway.poll() is not None:
                raise RuntimeError("compositor exited")
            time.sleep(0.05)
        assert sockets, "no Wayland socket"
        env["WAYLAND_DISPLAY"] = sockets[0].name
        app = subprocess.Popen(
            [binary],
            env=dict(env, WAYLAND_DEBUG="client"),
            stdout=applog,
            stderr=applog,
        )
        cases = [
            ("idle", {"phase": "idle"}),
            ("starting", {"phase": "starting"}),
            (
                "listening",
                {
                    "phase": "recording",
                    "recording_seconds": 12,
                    "input_level": 0.06,
                },
            ),
            (
                "destination",
                {
                    "phase": "recording",
                    "recording_seconds": 1,
                    "input_level": 0.03,
                },
            ),
            (
                "muted",
                {
                    "phase": "recording",
                    "recording_seconds": 12,
                    "microphone": {"muted": True},
                },
            ),
            (
                "clipping",
                {
                    "phase": "recording",
                    "recording_seconds": 12,
                    "input_level": 1,
                    "microphone": {"clipping": True},
                },
            ),
            ("transcribing", {"phase": "transcribing"}),
            (
                "retained",
                {
                    "phase": "idle",
                    "retained": True,
                    "retained_source": "Notes",
                },
            ),
            (
                "speaking",
                {"phase": "idle", "speaking": True, "audible": True},
            ),
            ("ready", {"phase": "draft", "draft": True}),
            (
                "edited",
                {
                    "phase": "draft",
                    "draft": True,
                    "draft_edited": True,
                    "osd": True,
                },
            ),
            ("cleared", {"phase": "idle", "draft_edited": True, "osd": True}),
            (
                "before-loss",
                {
                    "phase": "recording",
                    "recording_seconds": 15,
                    "input_level": 0.03,
                },
            ),
            ("connection-lost", {"ok": False}),
            ("hidden-again", {"phase": "idle"}),
        ]
        for name, status in cases:
            state = {
                "ok": True,
                "recording_label": "pi · Notes",
                "session_label": "Notes",
                **status,
            }
            time.sleep(2.0 if name == "clipping" else 1.5)
            assert app.poll() is None, f"app exited in {name}"
            subprocess.run(
                ["grim", str(out / f"{name}.png")],
                env=env,
                check=True,
                timeout=5,
            )
            if name == "retained":
                ppm = subprocess.check_output(
                    ["grim", "-t", "ppm", "-g", "430,18 420x104", "-"],
                    env=env,
                    timeout=5,
                )
                magic, dimensions, depth, pixels = ppm.split(b"\n", 3)
                assert (magic, dimensions, depth) == (
                    b"P6",
                    b"420 104",
                    b"255",
                )
                for x, y in ((0, 0), (419, 0), (0, 103), (419, 103)):
                    offset = (y * 420 + x) * 3
                    assert pixels[offset : offset + 3] == bytes(
                        (52, 60, 73)
                    ), "theme painted the transparent margin"
            if name in ("retained", "speaking", "hidden-again"):
                offset = (out / "app.log").stat().st_size
                time.sleep(0.5)
                extra = (out / "app.log").read_bytes()[offset:]
                assert b".frame(" not in extra, (
                    f"idle animation frames in {name}"
                )
            print("Captured", name, flush=True)
    finally:
        for process in (app, sway, bus):
            if process is not None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
        finished.set()
        thread.join(timeout=2)
        server.close()
        swaylog.close()
        applog.close()
    log = (out / "app.log").read_text()
    failures = [
        line
        for line in log.splitlines()
        if any(
            s in line for s in ("Traceback", "CRITICAL", "Error", "WARNING")
        )
    ]
    assert not failures, "\n".join(failures)
    assert (out / "idle.png").read_bytes() == (
        out / "hidden-again.png"
    ).read_bytes(), "pill did not hide"
    for name, _ in cases[1:-1]:
        is_hidden = (out / f"{name}.png").read_bytes() == (
            out / "idle.png"
        ).read_bytes()
        assert is_hidden == (name in ("edited", "cleared")), (
            f"wrong visibility: {name}"
        )
    assert "set_keyboard_interactivity(0)" in log, (
        "keyboard focus not disabled"
    )
    regions = {}
    inputs = []
    for line in log.splitlines():
        created = re.search(r"create_region\(new id wl_region#(\d+)\)", line)
        if created:
            regions[created[1]] = False
        added = re.search(r"wl_region#(\d+)\.add\(", line)
        if added:
            regions[added[1]] = True
        applied = re.search(r"set_input_region\(wl_region#(\d+)\)", line)
        if applied:
            inputs.append(not regions.get(applied[1], True))
    assert inputs and all(inputs), "surface has a nonempty input region"
    print(
        f"Native smoke: {len(cases)} visible/hidden states; "
        "empty input regions; no keyboard capture; no GTK warnings"
    )
    print("Synthetic compositor and app cleaned up")
