"""Check the local assistant's filesystem and public-web boundaries."""

import sys
import ssl
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

import httpcore

sys.path.insert(
    0, str(Path(__file__).resolve().parents[1] / "home/config/local-llm")
)

from assistant_tools import Workspace, fetch_page, validate_web_url


class WorkspaceTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.repo = self.base / "repos"
        self.output = self.base / "output"
        self.repo.mkdir()
        self.output.mkdir()
        self.workspace = Workspace([self.repo], self.output)
        (self.repo / "README.md").write_text(
            "A useful project\n", encoding="utf-8"
        )

    def test_reads_allowed_files_and_searches_content(self):
        self.assertIn(
            "useful", self.workspace.read(str(self.repo / "README.md"))
        )
        self.assertTrue(self.workspace.search("useful", str(self.repo)))

    def test_creates_and_reads_workspace_output(self):
        self.workspace.write("draft.txt", "A draft")
        self.assertEqual(self.workspace.read("draft.txt"), "A draft")

    def test_requires_explicit_overwrite(self):
        self.workspace.write("draft.txt", "original")
        with self.assertRaises(FileExistsError):
            self.workspace.write("draft.txt", "replacement")
        self.assertEqual(self.workspace.read("draft.txt"), "original")
        self.workspace.write("draft.txt", "replacement", overwrite=True)
        self.assertEqual(self.workspace.read("draft.txt"), "replacement")

    def test_cannot_write_into_read_only_repo(self):
        with self.assertRaises(PermissionError):
            self.workspace.write(
                str(self.repo / "README.md"), "replacement", True
            )

    def test_rejects_traversal_and_symlinks_outside_roots(self):
        secret = self.base / "private.txt"
        secret.write_text("private", encoding="utf-8")
        (self.repo / "escape").symlink_to(secret)
        (self.output / "escape").symlink_to(self.base, target_is_directory=True)
        for path in [str(secret), str(self.repo / "escape"), "../private.txt"]:
            with self.subTest(path=path), self.assertRaises(PermissionError):
                self.workspace.read(path)
        with self.assertRaises(PermissionError):
            self.workspace.write("escape/private.txt", "replacement", True)

    def test_sensitive_files_are_not_read_or_listed(self):
        (self.repo / ".env").write_text("password", encoding="utf-8")
        with self.assertRaises(PermissionError):
            self.workspace.read(str(self.repo / ".env"))
        self.assertNotIn(".env", str(self.workspace.list(str(self.repo))))
        self.assertEqual(self.workspace.search("password", str(self.repo)), [])


class PublicWebTest(unittest.TestCase):
    def stream(self, response):
        stream = httpcore.MockStream([response])
        stream.write = Mock(wraps=stream.write)
        stream.start_tls = Mock(wraps=stream.start_tls)
        return stream

    @patch("httpcore._backends.sync.SyncBackend.connect_tcp")
    @patch("assistant_tools.socket.getaddrinfo")
    def test_pins_connection_to_validated_ip_preserving_host_and_tls(
        self, resolver, connect
    ):
        # A second lookup of the hostname would let a rebinding domain change
        # the destination. Keep the actual HTTP and TLS setup in this test.
        resolver.side_effect = [
            [(2, 1, 6, "", ("93.184.216.34", 8443))],
            [(2, 1, 6, "", ("127.0.0.1", 8443))],
        ]
        stream = self.stream(
            b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nSafe"
        )
        connect.return_value = stream
        result = fetch_page("https://example.test:8443/page?q=1")
        self.assertEqual(result["text"], "Safe")
        self.assertEqual(result["url"], "https://example.test:8443/page?q=1")
        self.assertEqual(connect.call_args.kwargs["host"], "93.184.216.34")
        self.assertEqual(connect.call_args.kwargs["port"], 8443)
        self.assertEqual(resolver.call_count, 1)
        request = b"".join(call.args[0] for call in stream.write.call_args_list)
        self.assertIn(b"GET /page?q=1 HTTP/1.1", request)
        self.assertIn(b"Host: example.test:8443", request)
        tls = stream.start_tls.call_args.kwargs
        self.assertEqual(tls["server_hostname"], "example.test")
        self.assertTrue(tls["ssl_context"].check_hostname)
        self.assertEqual(tls["ssl_context"].verify_mode, ssl.CERT_REQUIRED)

    @patch("httpcore._backends.sync.SyncBackend.connect_tcp")
    @patch("assistant_tools.socket.getaddrinfo")
    def test_redirect_cannot_rebind_the_original_hostname(
        self, resolver, connect
    ):
        resolver.side_effect = [
            [(2, 1, 6, "", ("93.184.216.34", 80))],
            [(2, 1, 6, "", ("127.0.0.1", 80))],
        ]
        connect.return_value = self.stream(
            b"HTTP/1.1 302 Found\r\nLocation: /private\r\n"
            b"Content-Length: 0\r\nConnection: close\r\n\r\n"
        )
        with self.assertRaisesRegex(ValueError, "private network"):
            fetch_page("http://example.test/page")
        self.assertEqual(connect.call_count, 1)
        self.assertEqual(connect.call_args.kwargs["host"], "93.184.216.34")

    @patch("httpcore._backends.sync.SyncBackend.connect_tcp")
    @patch("assistant_tools.socket.getaddrinfo")
    def test_redirect_pins_its_own_public_destination(self, resolver, connect):
        resolver.side_effect = [
            [(2, 1, 6, "", ("93.184.216.34", 80))],
            [(2, 1, 6, "", ("1.1.1.1", 80))],
        ]
        connect.side_effect = [
            self.stream(
                b"HTTP/1.1 302 Found\r\nLocation: http://second.test/final\r\n"
                b"Content-Length: 0\r\nConnection: close\r\n\r\n"
            ),
            self.stream(b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nSafe"),
        ]
        result = fetch_page("http://example.test/page")
        self.assertEqual(result["url"], "http://second.test/final")
        self.assertEqual(
            [call.kwargs["host"] for call in connect.call_args_list],
            ["93.184.216.34", "1.1.1.1"],
        )

    @patch("httpcore._backends.sync.SyncBackend.connect_tcp")
    @patch("assistant_tools.socket.getaddrinfo")
    def test_same_ip_redirect_verifies_the_new_tls_hostname(
        self, resolver, connect
    ):
        resolver.return_value = [(2, 1, 6, "", ("93.184.216.34", 443))]
        first = self.stream(
            b"HTTP/1.1 302 Found\r\nLocation: https://second.test/final\r\n"
            b"Content-Length: 0\r\n\r\n"
        )
        second = self.stream(
            b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nSafe"
        )
        connect.side_effect = [first, second]
        self.assertEqual(fetch_page("https://first.test/page")["text"], "Safe")
        self.assertEqual(connect.call_count, 2)
        self.assertEqual(
            first.start_tls.call_args.kwargs["server_hostname"], "first.test"
        )
        self.assertEqual(
            second.start_tls.call_args.kwargs["server_hostname"], "second.test"
        )

    def test_rejects_local_and_non_http_urls(self):
        for url in [
            "file:///etc/passwd",
            "http://127.0.0.1",
            "http://192.168.178.1",
            "http://[::1]",
        ]:
            with self.subTest(url=url), self.assertRaises(ValueError):
                validate_web_url(url)

    @patch("assistant_tools.socket.getaddrinfo")
    def test_rejects_hostnames_resolving_to_private_addresses(self, resolver):
        resolver.return_value = [(2, 1, 6, "", ("10.0.0.1", 80))]
        with self.assertRaises(ValueError):
            validate_web_url("http://example.test")


if __name__ == "__main__":
    unittest.main()
