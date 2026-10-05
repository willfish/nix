"""Model assets and backend guards remain Python after the frontend cutover."""

import asyncio
import hashlib
import io
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import Mock, patch
import urllib.request

sys.path.insert(
    0, str(Path(__file__).resolve().parents[1] / "home/config/voice")
)
import personaplex_models as models
import personaplex_patch

PERSONAPLEX_URL = "http://127.0.0.1:8998"


class AssetTests(unittest.TestCase):
    def test_hash_and_size_are_both_checked(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "model"
            path.write_bytes(b"abc")
            digest = hashlib.sha256(b"abc").hexdigest()
            self.assertTrue(models.matches(path, 3, digest))
            self.assertFalse(models.matches(path, 4, digest))
            self.assertFalse(models.matches(path, 3, "0" * 64))

    def test_redirect_drops_token_on_another_host(self):
        request = urllib.request.Request(
            "https://huggingface.co/model",
            headers={"Authorization": "Bearer private"},
        )
        redirected = models.PrivateRedirect().redirect_request(
            request, None, 302, "", {}, "https://cdn.example/model"
        )
        self.assertFalse(redirected.has_header("Authorization"))

    def test_redirect_rejects_cleartext(self):
        request = urllib.request.Request("https://huggingface.co/model")
        with self.assertRaisesRegex(ValueError, "non-HTTPS"):
            models.PrivateRedirect().redirect_request(
                request, None, 302, "", {}, "http://cdn.example/model"
            )

    @patch.object(
        models,
        "token",
        side_effect=AssertionError("must not read credentials"),
    )
    def test_check_only_never_downloads(self, token):
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaisesRegex(ValueError, "Missing or invalid"):
                models.install(Path(root), check_only=True)

    def test_extract_rejects_traversal_and_links(self):
        for bad in ("../escape", "voices/../../escape", "voices/link"):
            with self.subTest(bad=bad), tempfile.TemporaryDirectory() as root:
                path = Path(root)
                with tarfile.open(path / "voices.tgz", "w:gz") as archive:
                    member = tarfile.TarInfo(bad)
                    if bad.endswith("link"):
                        member.type = tarfile.SYMTYPE
                        member.linkname = "/tmp/escape"
                    archive.addfile(member, io.BytesIO())
                with patch.object(models, "ASSETS", {}):
                    with self.assertRaisesRegex(ValueError, "Unsafe member"):
                        models.install(path)

    def test_server_patch_fails_closed_if_upstream_changes(self):
        with self.assertRaisesRegex(ValueError, "changed"):
            personaplex_patch.patch("different source")

    def test_server_guards_reject_untrusted_requests_before_websocket(self):
        source = (
            "    async def handle_chat(self, request):\n"
            "        ws = web.WebSocketResponse()"
        )
        web = Mock()

        # Keep this regression independent of the GPU runtime.
        class Denied(Exception):
            def __init__(self, *, text):
                super().__init__(text)

        web.HTTPForbidden = web.HTTPBadRequest = web.HTTPServiceUnavailable = (
            Denied
        )
        scope = {"web": web}
        exec("class State:\n" + personaplex_patch.patch(source), scope)
        state = scope["State"]()
        state.lock = Mock()
        for origin, voice, prompt, busy in (
            ("https://example.com", "NATF2.pt", "", False),
            (None, "NATF2.pt", "", False),
            (PERSONAPLEX_URL, "../../secret.pt", "", False),
            (PERSONAPLEX_URL, "NATF2.pt", "x" * 8001, False),
            (PERSONAPLEX_URL, "NATF2.pt", "", True),
        ):
            with self.subTest(origin=origin, voice=voice, busy=busy):
                state.lock.locked.return_value = busy
                request = Mock(
                    headers={"Origin": origin},
                    query={"voice_prompt": voice, "text_prompt": prompt},
                )
                with self.assertRaises(Denied):
                    asyncio.run(state.handle_chat(request))
        web.WebSocketResponse.assert_not_called()
        state.lock.locked.return_value = False
        asyncio.run(
            state.handle_chat(
                Mock(
                    headers={"Origin": PERSONAPLEX_URL},
                    query={"voice_prompt": "NATF2.pt"},
                )
            )
        )
        web.WebSocketResponse.assert_called_once_with()

    def test_browser_guard_injection_is_explicit_and_fails_on_root_drift(self):
        handler = (
            "    async def handle_chat(self, request):\n"
            "        ws = web.WebSocketResponse()"
        )
        with self.assertRaisesRegex(ValueError, "root changed"):
            personaplex_patch.patch(handler, "/guard.js")
        root = (
            "\n    def root(self):\n        return web.FileResponse("
            'os.path.join(static_path, "index.html"))'
        )
        patched = personaplex_patch.patch(handler + root, "/guard.js")
        self.assertIn('Path("/guard.js").read_text()', patched)
        self.assertIn('content_type="text/html"', patched)
        compile("class State:\n" + patched, "patched-server", "exec")

    def test_server_patch_guards_origin_and_voice_paths(self):
        source = (
            "    async def handle_chat(self, request):\n"
            "        ws = web.WebSocketResponse()"
        )
        result = personaplex_patch.patch(source)
        self.assertIn('request.headers.get("Origin")', result)
        self.assertIn("if voice not in allowed_voices:", result)
        self.assertIn("if self.lock.locked():", result)


if __name__ == "__main__":
    unittest.main()
