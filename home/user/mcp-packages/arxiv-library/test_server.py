import json
from pathlib import Path
import sys
import tempfile
import unittest

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
from library import add_paper, connect
from test_library import paper


class ServerTest(unittest.IsolatedAsyncioTestCase):
    async def test_real_mcp_lifecycle_tools_and_errors(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "library.sqlite3"
            db = connect(path, write=True)
            with db:
                add_paper(db, paper())
            db.close()
            params = StdioServerParameters(
                command=sys.executable,
                args=[
                    str(Path(__file__).with_name("server.py")),
                    "--database",
                    str(path),
                ],
            )
            async with stdio_client(params) as (read, write):
                async with ClientSession(read, write) as client:
                    await client.initialize()
                    names = {t.name for t in (await client.list_tools()).tools}
                    self.assertEqual(
                        names, {"search", "paper", "library_status"}
                    )
                    result = await client.call_tool(
                        "search", {"query": "rarephysicalterm"}
                    )
                    self.assertFalse(result.isError)
                    payload = json.loads(result.content[0].text)
                    self.assertEqual(
                        payload["papers"][0]["paper_id"], "test/001"
                    )
                    result = await client.call_tool(
                        "paper", {"paper_id": "test/001", "length": 20}
                    )
                    self.assertFalse(result.isError)
                    self.assertEqual(
                        json.loads(result.content[0].text)["next_offset"], 20
                    )
                    result = await client.call_tool("library_status", {})
                    self.assertEqual(
                        json.loads(result.content[0].text)["papers"], 1
                    )
                    result = await client.call_tool(
                        "paper", {"paper_id": "missing"}
                    )
                    self.assertTrue(result.isError)
                    result = await client.call_tool(
                        "search", {"query": "x", "limit": 100}
                    )
                    self.assertTrue(result.isError)


if __name__ == "__main__":
    unittest.main()
