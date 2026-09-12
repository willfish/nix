#!/usr/bin/env python3
"""Synthetic browser check only. Does not prove pre-login daemon operation."""
import os
from pathlib import Path
import tempfile

from playwright.sync_api import sync_playwright

executable = Path(os.environ["CHROME_PATH"])
assert str(executable).startswith("/nix/store/"), "Browser must be Nix-pinned"
assert executable.is_file() and os.access(executable, os.X_OK)
with tempfile.TemporaryDirectory(prefix="headless-browser-test-") as profile:
    with sync_playwright() as playwright:
        context = playwright.chromium.launch_persistent_context(
            profile, executable_path=str(executable),
            headless=True, timeout=30000,
        )
        try:
            context.route("**/*", lambda route: route.abort())
            page = context.new_page()
            page.set_content(
                "<title>Offline headless test</title><main>ready</main>"
            )
            assert page.title() == "Offline headless test"
            assert page.locator("main").inner_text() == "ready"
            assert page.evaluate("2 + 2") == 4
        finally:
            context.close()
print("Pinned headless browser passed with an offline disposable profile.")
