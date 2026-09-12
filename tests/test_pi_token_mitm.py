"""Offline tests for the Pi token MITM reporter."""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "home/config/llm/skills/pi-token-mitm/scripts/report.py"


def load_report():
    spec = importlib.util.spec_from_file_location(
        "pi_token_mitm_report", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ReportTests(unittest.TestCase):
    def setUp(self):
        self.report = load_report()
        self.rates = {
            "input": 2.0, "output": 6.0, "cacheRead": 0.5, "cacheWrite": 0.0}

    def fixture_rows(self, run_id="run-1"):
        tools = [
            {"type": "function", "name": "skill_catalog",
             "description": "find skills", "parameters": {"type": "object"}},
            {"type": "function", "name": "mcp",
             "description": "mcp status", "parameters": {"type": "object"}},
        ]
        call = {
            "type": "function_call",
            "name": "skill_catalog",
            "call_id": "call-1",
            "arguments": json.dumps({"query": "local development nix"}),
        }
        output = {
            "type": "function_call_output",
            "call_id": "call-1",
            "output": "local-dev-environment",
        }
        req1 = {
            "model": "grok-4.6",
            "tools": tools,
            "input": [
                {"role": "developer", "content": "instructions"},
                {"role": "user", "content": "turn 1"},
            ],
        }
        req2 = {
            "model": "grok-4.6",
            "tools": tools,
            "input": req1["input"] + [call, output],
        }
        return [
            {"run": run_id, "kind": "request", "flow_id": "f1",
             "ts": "t1", "url": "api.x.ai/v1/responses",
             "request_body": json.dumps(req1),
             "request_chars": len(json.dumps(req1))},
            {"run": run_id, "kind": "usage", "flow_id": "f1",
             "usage": {"input_tokens": 1000, "output_tokens": 20,
                       "total_tokens": 1020}},
            {"run": run_id, "kind": "response", "flow_id": "f1", "status": 200},
            {"run": run_id, "kind": "request", "flow_id": "f2",
             "ts": "t2", "url": "api.x.ai/v1/responses",
             "request_body": json.dumps(req2),
             "request_chars": len(json.dumps(req2))},
            {"run": run_id, "kind": "usage", "flow_id": "f2",
             "usage": {"input_tokens": 1100, "output_tokens": 10,
                       "cached_tokens": 800, "total_tokens": 1110,
                       "output_tokens_details": {"reasoning_tokens": 4}}},
            {"run": run_id, "kind": "response", "flow_id": "f2", "status": 200},
        ]

    def test_parses_responses_api_tool_calls_without_bodies_in_html(self):
        requests = self.report.build_requests(
            self.fixture_rows(), self.rates, "grok-4.6")
        self.assertEqual(len(requests), 2)
        self.assertEqual(requests[0]["tool_calls"], [])
        self.assertEqual(requests[1]["tool_calls"], ["skill_catalog"])
        self.assertEqual(
            requests[1]["skill_queries"], ["local development nix"])
        self.assertEqual(requests[1]["result_chars"]["skill_catalog"],
                         len("local-dev-environment"))
        attr = self.report.attribute_costs(requests, self.rates)
        self.assertEqual(attr["tools"]["skill_catalog"]["unique_calls"], 1)
        self.assertEqual(attr["tools"]["skill_catalog"]["wire_appearances"], 1)
        html = self.report.render_html(
            {"run_id": "run-1", "captured_at": "now",
             "model": "grok-4.6", "provider": "xai"},
            requests, attr, self.rates,
        )
        self.assertIn("Pi MITM token report", html)
        self.assertIn("aria-label=\"Tokens per LLM request\"", html)
        self.assertIn("skill_catalog", html)
        self.assertNotIn("request_body", html)
        self.assertNotIn("local-dev-environment", html)
        self.assertNotIn("instructions", html)

    def test_streaming_usage_keeps_max_and_costs_cache_reads(self):
        merged = self.report.merge_usage([
            {"usage": {"input_tokens": 100, "output_tokens": 1}},
            {"usage": {"input_tokens": 100, "output_tokens": 8,
                       "input_tokens_details": {"cached_tokens": 40}}},
        ])
        self.assertEqual(merged["input"], 100)
        self.assertEqual(merged["output"], 8)
        self.assertEqual(merged["cached"], 40)
        cost = self.report.cost_usd(merged, self.rates)
        expected = ((60 * 2.0) + (40 * 0.5) + (8 * 6.0)) / 1_000_000
        self.assertAlmostEqual(cost, expected)

    def test_write_report_archives_html_and_latest_symlink(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            capture = root / "pi.jsonl"
            rows = self.fixture_rows("abc")
            capture.write_text("\n".join(json.dumps(row) for row in rows))
            self.report.state_dir = lambda: root
            result = self.report.write_report(
                "abc",
                {"run_id": "abc", "captured_at": "now",
                 "model": "grok-4.6", "provider": "xai",
                 "task_result": "ok"},
                self.rates,
                capture_path=capture,
            )
            html_path = Path(result["html"])
            self.assertTrue(html_path.is_file())
            self.assertEqual(html_path.stat().st_mode & 0o777, 0o600)
            latest = html_path.parent / "latest.html"
            self.assertTrue(latest.is_symlink())
            self.assertEqual(latest.resolve(), html_path.resolve())
            self.assertEqual(result["request_count"], 2)

    def test_store_pi_bin_reads_wrapper_not_nested_capture(self):
        with tempfile.TemporaryDirectory() as tmp:
            wrapper = Path(tmp) / "pi"
            wrapper.write_text(
                "exec /nix/store/aaaa/bin/prompt-capture pi -- "
                "/nix/store/bbbb-pi-0.85.1/bin/pi --theme x \"$@\"\n"
            )
            self.assertEqual(
                self.report.store_pi_bin(wrapper),
                Path("/nix/store/bbbb-pi-0.85.1/bin/pi"),
            )

    def test_baseline_metadata_is_counts_only(self):
        baseline = json.loads(
            (SCRIPT.parent / "baseline-2026-09-12.json").read_text())
        self.assertEqual(baseline["run_id"], "20260912T104853-77612")
        self.assertEqual(baseline["request_count"], 7)
        blob = json.dumps(baseline)
        self.assertNotIn("request_body", blob)
        self.assertNotIn("Authorization", blob)


if __name__ == "__main__":
    unittest.main()
