"""Inject relay's API key for Tailscale clients only.

llama.cpp has one authentication setting for every listener. This proxy is the
public port. LAN and localhost clients must still present the key. Tailscale
clients are already authenticated by the tailnet, so they are not prompted.
"""

import argparse
import asyncio
import ipaddress
from pathlib import Path


TAILSCALE_NETS = (
    ipaddress.ip_network("100.64.0.0/10"),
    ipaddress.ip_network("fd7a:115c:a1e0::/48"),
)
HEADER_LIMIT = 65536


def tailscale_client(peer):
    try:
        address = ipaddress.ip_address(peer)
    except ValueError:
        return False
    mapped = getattr(address, "ipv4_mapped", None)
    if mapped is not None:
        address = mapped
    return any(address in network for network in TAILSCALE_NETS)


def _header_name(field):
    return field.split(b":", 1)[0].strip().lower()


def _bearer_token(field):
    value = field.split(b":", 1)[1].strip()
    if value.lower().startswith(b"bearer"):
        return value[6:].strip()
    return value


def rewrite_request(head, key, peer):
    """Return request headers, injecting the key only for a Tailscale client."""
    if not head.endswith(b"\r\n\r\n"):
        raise ValueError("incomplete request headers")
    lines = head[:-4].split(b"\r\n")
    start, fields = lines[0], lines[1:]
    websocket = False
    has_token = False
    kept = []
    for field in fields:
        if not field:
            continue
        name = _header_name(field)
        if name == b"connection":
            continue
        if name == b"upgrade" and b"websocket" in field.lower():
            websocket = True
        if name == b"authorization":
            if _bearer_token(field):
                has_token = True
                kept.append(field)
            continue
        kept.append(field)
    token = key.strip()
    if tailscale_client(peer) and token and not has_token:
        if any(char in token for char in "\r\n\x00"):
            raise ValueError("refusing a malformed API key")
        kept.append(b"Authorization: Bearer " + token.encode("ascii"))
    kept.append(b"Connection: Upgrade" if websocket else b"Connection: close")
    return start + b"\r\n" + b"\r\n".join(kept) + b"\r\n\r\n"


async def read_headers(reader):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = await reader.read(4096)
        if not chunk:
            break
        data += chunk
        if len(data) > HEADER_LIMIT:
            raise ValueError("request headers are too large")
    marker = data.find(b"\r\n\r\n")
    if marker < 0:
        return data, b""
    end = marker + 4
    return data[:end], data[end:]


async def _copy(source, destination):
    try:
        while True:
            chunk = await source.read(65536)
            if not chunk:
                break
            destination.write(chunk)
            await destination.drain()
    finally:
        try:
            destination.close()
        except Exception:
            pass


async def proxy_connection(client_reader, client_writer, upstream, key, peer):
    try:
        head, extra = await read_headers(client_reader)
        if not head.endswith(b"\r\n\r\n"):
            return
        rewritten = rewrite_request(head, key, peer)
        upstream_reader, upstream_writer = await asyncio.open_connection(
            *upstream
        )
    except Exception:
        try:
            client_writer.write(
                b"HTTP/1.1 502 Bad Gateway\r\n"
                b"Connection: close\r\nContent-Length: 0\r\n\r\n"
            )
            await client_writer.drain()
        except Exception:
            pass
        return
    try:
        upstream_writer.write(rewritten + extra)
        await upstream_writer.drain()
        tasks = {
            asyncio.create_task(_copy(client_reader, upstream_writer)),
            asyncio.create_task(_copy(upstream_reader, client_writer)),
        }
        done, pending = await asyncio.wait(
            tasks, return_when=asyncio.FIRST_COMPLETED
        )
        for task in pending:
            task.cancel()
        await asyncio.gather(*done, *pending, return_exceptions=True)
    finally:
        for writer in (upstream_writer, client_writer):
            try:
                writer.close()
            except Exception:
                pass


def load_key(path):
    return Path(path).read_text(encoding="ascii").strip()


async def serve(listen, upstream, key_file):
    key = load_key(key_file)

    async def handler(reader, writer):
        peer = writer.get_extra_info("peername")
        peer_ip = peer[0] if peer else ""
        await proxy_connection(reader, writer, upstream, key, peer_ip)

    server = await asyncio.start_server(handler, listen[0], listen[1])
    async with server:
        await server.serve_forever()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--listen-host", default="0.0.0.0")
    parser.add_argument("--listen-port", type=int, required=True)
    parser.add_argument("--upstream-host", default="127.0.0.1")
    parser.add_argument("--upstream-port", type=int, required=True)
    parser.add_argument("--key-file", required=True)
    args = parser.parse_args()
    asyncio.run(
        serve(
            (args.listen_host, args.listen_port),
            (args.upstream_host, args.upstream_port),
            args.key_file,
        )
    )


if __name__ == "__main__":
    main()
