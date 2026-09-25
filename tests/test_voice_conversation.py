import asyncio
import hashlib
import io
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import Mock, patch
import urllib.request

sys.path.insert(
    0, str(Path(__file__).resolve().parents[1] / "home/config/voice")
)
import voice_conversation as conversation
import voice_menu as menu
import personaplex_models as models
import personaplex_patch


class ConversationTests(unittest.TestCase):
    def test_active_mode_never_starts_pi_controller(self):
        mode = Mock()
        mode.active.return_value = True
        request = Mock()
        picker = Mock()
        menu.run_menu(request=request, picker=picker, conversation=mode)
        request.assert_not_called()
        mode.menu.assert_called_once_with(picker)

    def test_switch_from_pi_is_explicit_and_refreshed(self):
        mode = Mock()
        mode.active.return_value = False
        request = Mock(return_value={"phase": "idle"})
        menu.run_menu(
            request=request,
            picker=Mock(return_value="conversation:start"),
            conversation=mode,
        )
        mode.start.assert_called_once_with()
        self.assertEqual(request.call_count, 2)

    def test_stale_recording_blocks_switch(self):
        mode = Mock()
        mode.active.return_value = False
        request = Mock(side_effect=[{"phase": "idle"}, {"phase": "recording"}])
        with self.assertRaisesRegex(RuntimeError, "Finish or discard"):
            menu.run_menu(
                request=request,
                picker=Mock(return_value="conversation:start"),
                conversation=mode,
            )
        mode.start.assert_not_called()

    def test_private_pending_content_blocks_switch(self):
        for field in (
            "retained",
            "pending",
            "draft",
            "retry",
            "preparing_transcription",
        ):
            with self.subTest(field=field):
                state = {
                    "phase": "idle",
                    "conversation_available": True,
                    field: True,
                }
                self.assertFalse(conversation.can_switch(state))
                self.assertNotIn(
                    "conversation:start", dict(menu.rows_for(state, "menu"))
                )

    def test_feature_absent_on_other_hosts(self):
        self.assertNotIn(
            "conversation:start", dict(menu.rows_for({"phase": "idle"}, "menu"))
        )

    def test_cancel_changes_nothing(self):
        run = Mock()
        mode = conversation.Conversation(run)
        mode.menu(Mock(return_value=None))
        run.assert_not_called()

    def test_switch_back_stops_personaplex_before_starting_pi(self):
        run = Mock(return_value=Mock(stdout=""))
        mode = conversation.Conversation(run)
        mode.menu(Mock(return_value="pi"))
        self.assertEqual(
            run.call_args_list[0].args[0],
            [
                "systemctl",
                "--user",
                "stop",
                "personaplex-open.service",
                "personaplex.service",
            ],
        )
        self.assertEqual(
            run.call_args_list[1].args[0],
            [
                "systemctl",
                "--user",
                "start",
                "pi-voice.service",
                "pi-voice-osd.service",
            ],
        )

    def test_stop_failure_does_not_start_pi(self):
        run = Mock(side_effect=subprocess.CalledProcessError(1, "systemctl"))
        with self.assertRaises(subprocess.CalledProcessError):
            conversation.Conversation(run).stop()
        self.assertEqual(run.call_count, 1)

    def test_start_is_nonblocking_and_opens_ready_waiter(self):
        run = Mock(return_value=Mock(stdout=""))
        conversation.Conversation(run).start()
        self.assertIn("--no-block", run.call_args_list[0].args[0])
        self.assertEqual(
            run.call_args_list[1].args[0][-1], "personaplex-open.service"
        )

    def test_active_states(self):
        for state in (
            "active",
            "activating",
            "deactivating",
            "inactive",
            "failed",
        ):
            run = Mock(return_value=Mock(stdout=state))
            self.assertEqual(
                conversation.Conversation(run).active(),
                state in ("active", "activating", "deactivating"),
            )

    @patch.object(conversation.subprocess, "run")
    @patch.object(conversation.urllib.request, "urlopen")
    def test_browser_opens_only_after_readiness(self, urlopen, run):
        urlopen.return_value.__enter__.return_value.status = 200
        urlopen.return_value.__enter__.return_value.read.return_value = (
            b"<title>PersonaPlex</title>"
        )
        mode = Mock()
        mode.active.return_value = True
        conversation.open_when_ready(mode)
        run.assert_called_once_with(
            ["xdg-open", conversation.URL], check=True, timeout=15
        )

    @patch.object(conversation.time, "sleep")
    @patch.object(conversation.time, "monotonic", side_effect=[0, 0, 2])
    @patch.object(conversation.subprocess, "run")
    @patch.object(conversation.urllib.request, "urlopen")
    def test_unrelated_http_service_is_not_treated_as_ready(
        self, urlopen, run, clock, sleep
    ):
        response = urlopen.return_value.__enter__.return_value
        response.status = 200
        response.read.return_value = b"<title>Other service</title>"
        mode = Mock()
        mode.active.return_value = True
        with self.assertRaisesRegex(RuntimeError, "did not become ready"):
            conversation.open_when_ready(mode, timeout=1)
        run.assert_not_called()

    @patch.object(conversation.subprocess, "run")
    def test_backend_failure_does_not_open_browser(self, run):
        mode = Mock()
        mode.active.return_value = False
        with self.assertRaisesRegex(RuntimeError, "stopped or failed"):
            conversation.open_when_ready(mode)
        run.assert_not_called()


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
        models, "token", side_effect=AssertionError("must not read credentials")
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

        # aiohttp exceptions accept keyword text; these lightweight equivalents
        # keep this regression independent of the GPU runtime.
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
            (conversation.URL, "../../secret.pt", "", False),
            (conversation.URL, "NATF2.pt", "x" * 8001, False),
            (conversation.URL, "NATF2.pt", "", True),
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
                    headers={"Origin": conversation.URL},
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
            '\n    def root(self):\n        return web.FileResponse('
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
