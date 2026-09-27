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
        await self.lifecycle(separate_root=False)

    async def test_real_mcp_with_separate_canonical_root(self):
        await self.lifecycle(separate_root=True)

    async def lifecycle(self, separate_root):
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
            expected_index = None
            if separate_root:
                corpus = Path(tmp) / "canonical"
                folder = corpus / "embeddings"
                folder.mkdir(parents=True)
                # Only status metadata is needed: BM25 must not load a model
                # or ANN files, even when a retained generation is advertised.
                expected_index = {
                    "papers": 1,
                    "passages": 1,
                    "model": "retained-fixture",
                    "kind": "flat",
                }
                state = expected_index | {
                    "config": {"model": expected_index["model"]}
                }
                (folder / "index.json").write_text(json.dumps(state))
                params.args.extend(["--corpus-root", str(corpus)])
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
                    status = json.loads(result.content[0].text)
                    self.assertEqual(status["papers"], 1)
                    self.assertEqual(status["embedding_index"], expected_index)
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
