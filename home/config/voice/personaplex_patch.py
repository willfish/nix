"""Apply fail-closed localhost guards to the pinned upstream server."""

from pathlib import Path
import json
import sys


def patch(source, browser_guard=None):
    old = (
        "    async def handle_chat(self, request):\n"
        "        ws = web.WebSocketResponse()"
    )
    new = """    async def handle_chat(self, request):
        if request.headers.get("Origin") not in (
            "http://127.0.0.1:8998", "http://localhost:8998"
        ):
            raise web.HTTPForbidden(text="Local UI origin required")
        voice = request.query.get("voice_prompt", "")
        allowed_voices = {
            f"{prefix}{index}.pt"
            for prefix, count in (
                ("NATF", 4), ("NATM", 4), ("VARF", 5), ("VARM", 5)
            )
            for index in range(count)
        }
        if voice not in allowed_voices:
            raise web.HTTPBadRequest(text="Unknown voice")
        if len(request.query.get("text_prompt", "")) > 8000:
            raise web.HTTPBadRequest(text="Prompt too long")
        if self.lock.locked():
            raise web.HTTPServiceUnavailable(text="Conversation already active")
        ws = web.WebSocketResponse()"""
    if source.count(old) != 1:
        raise ValueError(
            "Pinned PersonaPlex server changed; review origin guards"
        )
    source = source.replace(old, new)
    if browser_guard is not None:
        old_root = (
            'return web.FileResponse(os.path.join(static_path, "index.html"))'
        )
        new_root = (
            'return web.Response(text=Path(os.path.join('
            'static_path, "index.html")).read_text()'
            '.replace("<head>", "<head><script>" + Path('
            + json.dumps(str(browser_guard))
            + ').read_text() + "</script>", 1), content_type="text/html")'
        )
        if source.count(old_root) != 1:
            raise ValueError(
                "Pinned PersonaPlex root changed; review microphone cleanup"
            )
        source = source.replace(old_root, new_root)
    # Upstream looks up the seed on the request object rather than its query.
    return source.replace('int(request["seed"])', 'int(request.query["seed"])')


if __name__ == "__main__":
    path = Path(sys.argv[1])
    path.write_text(patch(path.read_text(), sys.argv[2]))
