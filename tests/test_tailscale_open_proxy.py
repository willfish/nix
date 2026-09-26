import asyncio
import socket
import unittest

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1] / "home/config/local-llm"
sys.path.insert(0, str(ROOT))
from tailscale_open_proxy import (  # noqa: E402
    proxy_connection,
    rewrite_request,
    tailscale_client,
)


class TailscaleClientTests(unittest.TestCase):
    def test_accepts_tailnet_addresses_only(self):
        self.assertTrue(tailscale_client("100.94.75.75"))
        self.assertTrue(tailscale_client("100.106.132.35"))
        self.assertTrue(tailscale_client("fd7a:115c:a1e0::12"))
        self.assertTrue(tailscale_client("::ffff:100.94.75.75"))
        self.assertFalse(tailscale_client("192.168.178.57"))
        self.assertFalse(tailscale_client("127.0.0.1"))
        self.assertFalse(tailscale_client("::1"))
        self.assertFalse(tailscale_client("not-an-ip"))


class RewriteTests(unittest.TestCase):
    def test_injects_for_tailscale_and_closes_http(self):
        head = (
            b"GET /props HTTP/1.1\r\n"
            b"Host: relay\r\nConnection: keep-alive\r\n\r\n"
        )
        rewritten = rewrite_request(head, "secret", "100.94.75.75")
        self.assertIn(b"Authorization: Bearer secret\r\n", rewritten)
        self.assertIn(b"Connection: close\r\n", rewritten)
        self.assertNotIn(b"keep-alive", rewritten)

    def test_leaves_lan_and_existing_tokens_alone(self):
        head = (
            b"GET /props HTTP/1.1\r\nHost: relay\r\n"
            b"Authorization: Bearer client\r\n\r\n"
        )
        lan = rewrite_request(head, "secret", "192.168.178.20")
        self.assertNotIn(b"secret", lan)
        self.assertIn(b"Authorization: Bearer client\r\n", lan)
        kept = rewrite_request(head, "secret", "100.1.2.3")
        self.assertEqual(kept.count(b"Authorization:"), 1)
        self.assertIn(b"Bearer client", kept)

    def test_replaces_an_empty_bearer_and_preserves_websocket(self):
        head = (
            b"GET /mcp HTTP/1.1\r\nHost: relay\r\n"
            b"Authorization: Bearer\r\nUpgrade: websocket\r\n"
            b"Connection: keep-alive, Upgrade\r\n\r\n"
        )
        rewritten = rewrite_request(head, "secret", "fd7a:115c:a1e0::1")
        self.assertIn(b"Authorization: Bearer secret\r\n", rewritten)
        self.assertIn(b"Connection: Upgrade\r\n", rewritten)
        self.assertIn(b"Upgrade: websocket\r\n", rewritten)


class ProxyTests(unittest.TestCase):
    def test_forwards_an_injected_request_and_response(self):
        seen = {}

        async def upstream(reader, writer):
            seen["request"] = await reader.read(4096)
            writer.write(
                b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                b"Connection: close\r\n\r\nok"
            )
            await writer.drain()
            writer.close()

        async def scenario():
            server = await asyncio.start_server(upstream, "127.0.0.1", 0)
            port = server.sockets[0].getsockname()[1]
            left, right = socket.socketpair()
            left.setblocking(False)
            right.setblocking(False)
            proxy_reader, proxy_writer = await asyncio.open_connection(
                sock=left
            )
            client_reader, client_writer = await asyncio.open_connection(
                sock=right
            )
            task = asyncio.create_task(
                proxy_connection(
                    proxy_reader,
                    proxy_writer,
                    ("127.0.0.1", port),
                    "secret",
                    "100.94.75.75",
                )
            )
            client_writer.write(b"GET /props HTTP/1.1\r\nHost: relay\r\n\r\n")
            await client_writer.drain()
            response = b""
            while b"ok" not in response:
                chunk = await asyncio.wait_for(client_reader.read(4096), 2)
                if not chunk:
                    break
                response += chunk
            await asyncio.wait_for(task, 2)
            server.close()
            await server.wait_closed()
            return response

        response = asyncio.run(scenario())
        self.assertIn(b"Authorization: Bearer secret", seen["request"])
        self.assertIn(b"200 OK", response)
        self.assertTrue(response.endswith(b"ok"))


if __name__ == "__main__":
    unittest.main()
