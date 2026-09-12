#!/usr/bin/env python3
"""Build a Pi prompt-capture token report. Keep bodies out of the HTML."""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

DEFAULT_RATES = {
    "input": 2.0, "output": 6.0, "cacheRead": 0.5, "cacheWrite": 0.0,
}
DEFAULT_MODEL = "grok-4.6"
DEFAULT_PROVIDER = "xai"
TURNS = [
    (
        "turn-1-skill-catalog",
        "Work only in the current directory. Do not use team, subagent, "
        "or edit any files. Call skill_catalog once with query "
        "'local development nix'. After you have the catalog result, "
        "reply with the matching skill name only and stop.",
    ),
    (
        "turn-2-mcp-status",
        "Work only in the current directory. Do not use team or subagent. "
        "Call the mcp tool with no arguments so it lists MCP server status. "
        "Reply with connected server names only, then stop. "
        "Do not start other work.",
    ),
    (
        "turn-3-write-and-run",
        "Work only in the current directory. Do not use team or subagent. "
        "Create add.py that prints the integer result of 2+3. Run it with "
        "python3 add.py. Stop when stdout is 5. Do not do anything else.",
    ),
]


def state_dir() -> Path:
    root = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state"))
    return root / "prompt-capture"


def reports_dir() -> Path:
    path = state_dir() / "reports"
    path.mkdir(parents=True, exist_ok=True)
    path.chmod(0o700)
    return path


def jsonl_path() -> Path:
    return state_dir() / "pi.jsonl"


def first_int(obj: dict[str, Any], *keys: str) -> int | None:
    for key in keys:
        val = obj.get(key)
        if isinstance(val, bool):
            continue
        if isinstance(val, (int, float)):
            return int(val)
    return None


def usage_fields(usage: dict[str, Any]) -> dict[str, int]:
    if not isinstance(usage, dict):
        return {}
    input_tokens = first_int(
        usage, "input_tokens", "prompt_tokens", "inputTokens", "promptTokens")
    output_tokens = first_int(
        usage, "output_tokens", "completion_tokens", "outputTokens",
        "completionTokens")
    total_tokens = first_int(usage, "total_tokens", "totalTokens")
    cached = 0
    reasoning = 0
    details = usage.get("input_tokens_details") or usage.get(
        "prompt_tokens_details") or {}
    if isinstance(details, dict):
        cached = first_int(details, "cached_tokens", "cachedTokens") or 0
    out_details = usage.get("output_tokens_details") or usage.get(
        "completion_tokens_details") or {}
    if isinstance(out_details, dict):
        reasoning = first_int(
            out_details, "reasoning_tokens", "reasoningTokens") or 0
    cached = first_int(
        usage, "cached_tokens", "cache_read_input_tokens") or cached
    if not total_tokens and (input_tokens or output_tokens):
        total_tokens = (input_tokens or 0) + (output_tokens or 0)
    out: dict[str, int] = {}
    if input_tokens:
        out["input"] = input_tokens
    if output_tokens:
        out["output"] = output_tokens
    if total_tokens:
        out["total"] = total_tokens
    if cached:
        out["cached"] = cached
    if reasoning:
        out["reasoning"] = reasoning
    return out


def merge_usage(records: list[dict[str, Any]]) -> dict[str, int]:
    merged: dict[str, int] = {}
    for rec in records:
        for key, val in usage_fields(rec.get("usage") or {}).items():
            merged[key] = max(merged.get(key, 0), val)
    return merged


def cost_usd(fields: dict[str, int], rates: dict[str, float]) -> float:
    inp = fields.get("input", 0)
    cached = min(fields.get("cached", 0), inp)
    uncached = max(inp - cached, 0)
    output = fields.get("output", 0)
    return (
        uncached * rates["input"]
        + cached * rates["cacheRead"]
        + output * rates["output"]
    ) / 1_000_000


def input_cost_usd(usage: dict[str, int], rates: dict[str, float]) -> float:
    inp = usage.get("input", 0)
    cached = min(usage.get("cached", 0), inp)
    uncached = max(inp - cached, 0)
    return (uncached * rates["input"] + cached * rates["cacheRead"]) / 1_000_000


def tool_name(tool: Any) -> str | None:
    if not isinstance(tool, dict):
        return None
    for key in ("name", "tool"):
        val = tool.get(key)
        if isinstance(val, str) and val:
            return val
    fn = tool.get("function") or tool.get("definition") or {}
    if isinstance(fn, dict) and isinstance(fn.get("name"), str):
        return fn["name"]
    return None


def extract_call(call: Any) -> dict[str, str] | None:
    if not isinstance(call, dict):
        return None
    fn = (
        call.get("function") if isinstance(call.get("function"), dict)
        else call
    )
    name = fn.get("name") or call.get("name") or call.get("tool")
    args = (
        fn.get("arguments") or call.get("arguments")
        or call.get("input") or ""
    )
    if not isinstance(name, str):
        return None
    if isinstance(args, (dict, list)):
        args = json.dumps(args, ensure_ascii=False)
    if not isinstance(args, str):
        args = str(args)
    return {"name": name, "arguments": args}


def record_call(call: dict[str, str], info: dict[str, Any]) -> None:
    name = call["name"]
    info["tool_calls"].append(name)
    args = call.get("arguments") or ""
    if name == "skill_catalog":
        query = ""
        try:
            parsed = json.loads(args) if args else {}
            if isinstance(parsed, dict):
                query = str(parsed.get("query") or "")
        except ValueError:
            query = args[:80]
        if query:
            info["skill_queries"].append(query)
    if name == "mcp":
        info["mcp_calls"].append(args if args else "{}")


def scan_messages(messages: list[Any], info: dict[str, Any]) -> None:
    id_to_name: dict[str, str] = {}
    for msg in messages:
        if not isinstance(msg, dict):
            continue
        if msg.get("role") == "developer":
            content = msg.get("content")
            if isinstance(content, str):
                info["instruction_chars"] += len(content)
            elif content is not None:
                info["instruction_chars"] += len(
                    json.dumps(content, ensure_ascii=False))
        if msg.get("type") == "function_call":
            called = extract_call(msg)
            if called:
                record_call(called, info)
                cid = msg.get("call_id") or msg.get("id")
                if isinstance(cid, str):
                    id_to_name[cid] = called["name"]
                    info["call_ids"][cid] = called["name"]
        tool_calls = msg.get("tool_calls") or msg.get("toolCalls") or []
        if isinstance(tool_calls, list):
            for call in tool_calls:
                called = extract_call(call)
                if called:
                    record_call(called, info)
    for msg in messages:
        if not isinstance(msg, dict):
            continue
        if msg.get("type") == "function_call_output":
            tname = id_to_name.get(str(msg.get("call_id") or ""))
            if tname:
                info["tool_results"].append(tname)
                out = msg.get("output")
                size = (
                    len(out) if isinstance(out, str)
                    else len(json.dumps(out, ensure_ascii=False))
                    if out is not None else 0
                )
                info["result_chars"][tname] = (
                    info["result_chars"].get(tname, 0) + size
                )


def parse_payload(body: str) -> dict[str, Any]:
    info: dict[str, Any] = {
        "tools_advertised": [],
        "tool_schema_chars": 0,
        "message_chars": 0,
        "instruction_chars": 0,
        "tool_calls": [],
        "tool_results": [],
        "call_ids": {},
        "result_chars": {},
        "skill_queries": [],
        "mcp_calls": [],
        "message_count": 0,
        "model": None,
    }
    try:
        payload = json.loads(body)
    except (ValueError, TypeError):
        info["message_chars"] = len(body or "")
        return info
    if not isinstance(payload, dict):
        info["message_chars"] = len(body or "")
        return info
    info["model"] = payload.get("model")
    tools = payload.get("tools") or payload.get("functions") or []
    if isinstance(tools, list):
        raw = json.dumps(tools, ensure_ascii=False, separators=(",", ":"))
        info["tool_schema_chars"] = len(raw)
        for tool in tools:
            name = tool_name(tool)
            if name:
                info["tools_advertised"].append(name)
    for key in ("instructions", "system"):
        val = payload.get(key)
        if isinstance(val, str):
            info["instruction_chars"] += len(val)
        elif isinstance(val, list):
            info["instruction_chars"] += len(
                json.dumps(val, ensure_ascii=False))
    messages = payload.get("messages") or payload.get("input") or []
    if isinstance(messages, list):
        info["message_count"] = len(messages)
        info["message_chars"] = len(json.dumps(messages, ensure_ascii=False))
        scan_messages(messages, info)
    return info


def looks_like_llm(url: str, body: str) -> bool:
    lowered = url.lower()
    if any(part in lowered for part in (
            "/chat/completions", "/responses", "/v1/messages", "/v1/chat",
            "api.x.ai")):
        return True
    try:
        payload = json.loads(body)
    except (ValueError, TypeError):
        return False
    return isinstance(payload, dict) and (
        "messages" in payload or "input" in payload or "tools" in payload
        or payload.get("model")
    )


def load_records(path: Path, run_id: str) -> list[dict[str, Any]]:
    rows = []
    if not path.exists():
        return rows
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        try:
            rec = json.loads(line)
        except ValueError:
            continue
        if rec.get("run") == run_id:
            rows.append(rec)
    return rows


def build_requests(
    rows: list[dict[str, Any]], rates: dict[str, float], model: str
) -> list[dict[str, Any]]:
    by_flow: dict[str, dict[str, Any]] = {}
    order: list[str] = []
    for rec in rows:
        fid = rec.get("flow_id") or rec.get("ts")
        if fid not in by_flow:
            by_flow[fid] = {
                "flow_id": fid, "usages": [], "request": None, "response": None}
            order.append(fid)
        kind = rec.get("kind")
        if kind == "usage":
            by_flow[fid]["usages"].append(rec)
        elif kind == "request" and by_flow[fid]["request"] is None:
            by_flow[fid]["request"] = rec
        elif kind == "response":
            by_flow[fid]["response"] = rec
    requests = []
    idx = 0
    for fid in order:
        item = by_flow[fid]
        req = item["request"] or {}
        url = req.get("url") or ""
        body = req.get("request_body") or ""
        if req and not looks_like_llm(url, body):
            continue
        if not req and not item["usages"]:
            continue
        idx += 1
        parsed = parse_payload(body) if body else parse_payload("{}")
        fields = merge_usage(item["usages"])
        requests.append({
            "index": idx,
            "ts": req.get("ts") or (
                item["usages"][0].get("ts") if item["usages"] else ""),
            "url": url,
            "status": (item["response"] or {}).get("status"),
            "model": parsed.get("model") or req.get("model") or model,
            "request_chars": req.get("request_chars") or len(body),
            "message_count": parsed.get("message_count") or 0,
            "tool_count": len(parsed.get("tools_advertised") or []),
            "tools_advertised": parsed.get("tools_advertised") or [],
            "tool_schema_chars": parsed.get("tool_schema_chars") or 0,
            "tool_calls": parsed.get("tool_calls") or [],
            "tool_results": parsed.get("tool_results") or [],
            "call_ids": parsed.get("call_ids") or {},
            "result_chars": parsed.get("result_chars") or {},
            "skill_queries": parsed.get("skill_queries") or [],
            "mcp_calls": parsed.get("mcp_calls") or [],
            "usage": fields,
            "cost_usd": cost_usd(fields, rates),
        })
    return requests


def attribute_costs(
    requests: list[dict[str, Any]], rates: dict[str, float]
) -> dict[str, Any]:
    tools: dict[str, dict[str, Any]] = defaultdict(lambda: {
        "unique_calls": 0,
        "wire_appearances": 0,
        "result_appearances": 0,
        "result_chars": 0,
        "cost_usd": 0.0,
    })
    skills: dict[str, dict[str, Any]] = defaultdict(
        lambda: {"queries": [], "unique_calls": 0, "wire_appearances": 0,
                 "cost_usd": 0.0})
    mcp: dict[str, dict[str, Any]] = defaultdict(
        lambda: {"notes": [], "unique_calls": 0, "wire_appearances": 0,
                 "cost_usd": 0.0})
    advertised: dict[str, int] = {}
    seen_calls: dict[str, str] = {}
    schema_chars_total = 0
    schema_cost = 0.0
    for req in requests:
        chars = max(req["request_chars"], 1)
        in_cost = input_cost_usd(req["usage"], rates)
        schema_chars_total += req["tool_schema_chars"]
        schema_cost += in_cost * (req["tool_schema_chars"] / chars)
        for name in req["tools_advertised"]:
            advertised[name] = advertised.get(name, 0) + 1
        for call_id, name in (req.get("call_ids") or {}).items():
            if call_id not in seen_calls:
                seen_calls[call_id] = name
                tools[name]["unique_calls"] += 1
        for name in req["tool_calls"]:
            tools[name]["wire_appearances"] += 1
        for name in req["tool_results"]:
            tools[name]["result_appearances"] += 1
        result_chars = req.get("result_chars") or {}
        for name, size in result_chars.items():
            tools[name]["result_chars"] += size
        rest = max(in_cost * (1 - req["tool_schema_chars"] / chars), 0)
        total_rc = sum(result_chars.values())
        if rest and total_rc:
            for name, size in result_chars.items():
                tools[name]["cost_usd"] += rest * (size / total_rc)
        elif rest and req["tool_calls"]:
            names = list(dict.fromkeys(req["tool_calls"]))
            share = rest / len(names)
            for name in names:
                tools[name]["cost_usd"] += share
        for query in req["skill_queries"]:
            if query and query not in skills["skill_catalog"]["queries"]:
                skills["skill_catalog"]["queries"].append(query)
        for note in req["mcp_calls"]:
            if note and note not in mcp["mcp"]["notes"]:
                mcp["mcp"]["notes"].append(note)
    for name, data in tools.items():
        if name == "skill_catalog":
            skills[name]["unique_calls"] = data["unique_calls"]
            skills[name]["wire_appearances"] = data["wire_appearances"]
            skills[name]["cost_usd"] = data["cost_usd"]
        if name == "mcp":
            mcp[name]["unique_calls"] = data["unique_calls"]
            mcp[name]["wire_appearances"] = data["wire_appearances"]
            mcp[name]["cost_usd"] = data["cost_usd"]
    return {
        "tools": dict(tools),
        "skills": dict(skills),
        "mcp": dict(mcp),
        "advertised": advertised,
        "schema_chars_total": schema_chars_total,
        "schema_cost_usd": schema_cost,
    }


def html_escape(text: Any) -> str:
    return (
        str(text).replace("&", "&amp;").replace("<", "&lt;")
        .replace(">", "&gt;").replace('"', "&quot;")
    )


def svg_histogram(requests: list[dict[str, Any]]) -> str:
    if not requests:
        return "<p>No LLM requests captured.</p>"
    width = max(720, 80 + len(requests) * 70)
    height = 360
    left, right, top, bottom = 64, 24, 24, 56
    plot_w = width - left - right
    plot_h = height - top - bottom
    max_val = max(
        (r["usage"].get("input", 0) + r["usage"].get("output", 0))
        for r in requests
    ) or 1
    bar_w = plot_w / max(len(requests), 1) * 0.7
    gap = plot_w / max(len(requests), 1)
    parts = [
        f'<svg viewBox="0 0 {width} {height}" width="100%" role="img" '
        'aria-label="Tokens per LLM request">'
        f'<rect width="{width}" height="{height}" fill="#0f1419"/>'
    ]
    for frac in (0, 0.25, 0.5, 0.75, 1.0):
        y = top + plot_h * (1 - frac)
        val = int(max_val * frac)
        parts.append(
            f'<line x1="{left}" y1="{y:.1f}" x2="{width-right}" y2="{y:.1f}" '
            'stroke="#243040" stroke-width="1"/>'
        )
        parts.append(
            f'<text x="{left-8}" y="{y+4:.1f}" text-anchor="end" '
            f'fill="#8aa0b4" font-size="11" '
            f'font-family="ui-sans-serif,system-ui">{val:,}</text>'
        )
    for i, req in enumerate(requests):
        cached = min(
            req["usage"].get("cached", 0), req["usage"].get("input", 0))
        uncached = max(req["usage"].get("input", 0) - cached, 0)
        output = req["usage"].get("output", 0)
        x = left + gap * i + (gap - bar_w) / 2

        def bar_h(v: int) -> float:
            return plot_h * (v / max_val)

        y = top + plot_h
        for val, color in (
            (uncached, "#3b82f6"), (cached, "#22c55e"), (output, "#f59e0b")
        ):
            if val <= 0:
                continue
            bh = bar_h(val)
            y -= bh
            parts.append(
                f'<rect x="{x:.1f}" y="{y:.1f}" width="{bar_w:.1f}" '
                f'height="{bh:.1f}" fill="{color}" rx="2"/>'
            )
        parts.append(
            f'<text x="{x + bar_w/2:.1f}" y="{height-28}" text-anchor="middle" '
            f'fill="#c5d4e3" font-size="11" '
            f'font-family="ui-sans-serif,system-ui">R{req["index"]}</text>'
        )
        total = req["usage"].get("input", 0) + req["usage"].get("output", 0)
        parts.append(
            f'<text x="{x + bar_w/2:.1f}" '
            f'y="{top + plot_h - bar_h(total) - 6:.1f}" '
            'text-anchor="middle" fill="#e8eef5" font-size="10" '
            f'font-family="ui-sans-serif,system-ui">{total:,}</text>'
        )
    parts.append(
        f'<text x="{left}" y="{height-8}" fill="#8aa0b4" font-size="12" '
        'font-family="ui-sans-serif,system-ui">'
        "Blue: uncached input · Green: cached input · Amber: output</text>"
    )
    parts.append("</svg>")
    return "\n".join(parts)


def render_html(
    meta: dict[str, Any],
    requests: list[dict[str, Any]],
    attr: dict[str, Any],
    rates: dict[str, float],
) -> str:
    totals: dict[str, int] = defaultdict(int)
    for req in requests:
        for key, val in req["usage"].items():
            totals[key] += val
    total_cost = sum(r["cost_usd"] for r in requests)
    rows = []
    for req in requests:
        usage = req["usage"]
        calls = ", ".join(req["tool_calls"]) or "—"
        rows.append(
            "<tr>"
            f"<td>{req['index']}</td>"
            f"<td>{html_escape(req['ts'])}</td>"
            f"<td>{usage.get('input', 0):,}</td>"
            f"<td>{usage.get('cached', 0):,}</td>"
            f"<td>{usage.get('output', 0):,}</td>"
            f"<td>{usage.get('reasoning', 0):,}</td>"
            f"<td>{req['request_chars']:,}</td>"
            f"<td>{req['tool_count']}</td>"
            f"<td>{req['message_count']}</td>"
            f"<td>${req['cost_usd']:.4f}</td>"
            f"<td>{html_escape(calls)}</td>"
            "</tr>"
        )
    names = sorted(
        set(attr["advertised"]) | set(attr["tools"]),
        key=lambda n: (-attr["tools"].get(n, {}).get("unique_calls", 0), n),
    )
    tool_rows = []
    for name in names:
        data = attr["tools"].get(name, {})
        advertised = attr["advertised"].get(name, 0)
        kind = "tool"
        note = f"advertised on {advertised} requests"
        if name == "skill_catalog":
            kind = "skill"
            note = ", ".join(
                attr["skills"].get(name, {}).get("queries") or []
            ) or "catalogue lookup"
        elif name == "mcp":
            kind = "mcp"
            note = "; ".join(
                attr["mcp"].get(name, {}).get("notes") or []
            ) or "status"
        tool_rows.append(
            "<tr>"
            f"<td>{html_escape(name)}</td>"
            f"<td>{kind}</td>"
            f"<td>{data.get('unique_calls', 0)}</td>"
            f"<td>{data.get('wire_appearances', 0)}</td>"
            f"<td>{html_escape(note)}</td>"
            f"<td>${data.get('cost_usd', 0.0):.4f}</td>"
            "</tr>"
        )
    if not tool_rows:
        tool_rows.append(
            "<tr><td colspan='6'>No skill, MCP, or tool invocations parsed."
            "</td></tr>"
        )
    turn_list = "".join(f"<li>{html_escape(name)}</li>" for name, _ in TURNS)
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8"/>
  <title>Pi MITM token report</title>
  <style>
    :root {{ color-scheme: dark; }}
    body {{ margin: 0; font-family: ui-sans-serif, system-ui, sans-serif;
           background: #0b1014; color: #e8eef5; line-height: 1.45; }}
    main {{ max-width: 1100px; margin: 0 auto; padding: 32px 20px 64px; }}
    h1, h2 {{ font-weight: 600; }}
    h1 {{ font-size: 1.6rem; margin-bottom: 0.3rem; }}
    .muted {{ color: #8aa0b4; }}
    .cards {{ display: grid; gap: 12px; margin: 20px 0 28px;
              grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); }}
    .card {{ background: #151c24; border: 1px solid #243040;
             border-radius: 10px; padding: 14px 16px; }}
    .card .n {{ font-size: 1.35rem; font-variant-numeric: tabular-nums; }}
    table {{ width: 100%; border-collapse: collapse; font-size: 0.92rem; }}
    th, td {{ text-align: left; padding: 8px 10px;
              border-bottom: 1px solid #243040;
              vertical-align: top; font-variant-numeric: tabular-nums; }}
    th {{ color: #8aa0b4; font-weight: 600; }}
    .note {{ background: #151c24; border-left: 3px solid #3b82f6;
             padding: 12px 14px; margin: 16px 0; }}
    svg {{ margin: 8px 0 24px; }}
  </style>
</head>
<body>
<main>
  <h1>Pi MITM token report</h1>
  <p class="muted">Captured {html_escape(meta.get('captured_at'))}.
  Model {html_escape(meta.get('model'))}
  via {html_escape(meta.get('provider'))}.
  Run {html_escape(meta.get('run_id'))}.
  Prices are list rates: input ${rates['input']}/M,
  cached ${rates['cacheRead']}/M,
  output ${rates['output']}/M.</p>
  <div class="cards">
    <div class="card"><div class="muted">LLM requests</div>
      <div class="n">{len(requests)}</div></div>
    <div class="card"><div class="muted">Input tokens sent</div>
      <div class="n">{totals.get('input', 0):,}</div></div>
    <div class="card"><div class="muted">Cached input</div>
      <div class="n">{totals.get('cached', 0):,}</div></div>
    <div class="card"><div class="muted">Output tokens</div>
      <div class="n">{totals.get('output', 0):,}</div></div>
    <div class="card"><div class="muted">Estimated USD</div>
      <div class="n">${total_cost:.4f}</div></div>
  </div>
  <div class="note">
    Bars are provider-reported tokens on each intercepted LLM HTTP request.
    Summing input across turns counts every resend of growing context.
    Tool and skill dollar figures are character-share estimates of input cost.
    Prompt bodies are omitted from this file.
  </div>
  <h2>Tokens per request</h2>
  {svg_histogram(requests)}
  <h2>User turns</h2>
  <ol>{turn_list}</ol>
  <p class="muted">Task result:
  {html_escape(meta.get('task_result') or 'unknown')}.</p>
  <h2>Request table</h2>
  <table>
    <thead><tr>
      <th>#</th><th>Time</th><th>Input</th><th>Cached</th><th>Output</th>
      <th>Reasoning</th><th>Request chars</th><th>Tools advertised</th>
      <th>Messages</th><th>Est. USD</th><th>Invoked</th>
    </tr></thead>
    <tbody>
      {''.join(rows) or '<tr><td colspan="11">No requests.</td></tr>'}
    </tbody>
  </table>
  <h2>Skills, MCP, and tools</h2>
  <p class="muted">Unique calls are distinct tool uses. Wire appearances count
  every resend of that call in later requests. Tool schemas across requests:
  {attr['schema_chars_total']:,} characters,
  about ${attr['schema_cost_usd']:.4f}
  of input cost.</p>
  <table>
    <thead><tr>
      <th>Name</th><th>Kind</th><th>Unique calls</th><th>Wire appearances</th>
      <th>Note</th><th>Est. USD</th>
    </tr></thead>
    <tbody>
      {''.join(tool_rows)}
    </tbody>
  </table>
</main>
</body>
</html>
"""


def load_rates(model: str = DEFAULT_MODEL) -> dict[str, float]:
    store = Path.home() / ".pi/agent/models-store.json"
    if not store.exists():
        return dict(DEFAULT_RATES)
    try:
        payload = json.loads(store.read_text())
    except (ValueError, OSError):
        return dict(DEFAULT_RATES)
    models = (payload.get("xai") or {}).get("models") or []
    for item in models:
        if isinstance(item, dict) and item.get("id") == model:
            cost = item.get("cost") or {}
            return {
                "input": float(cost.get("input", DEFAULT_RATES["input"])),
                "output": float(cost.get("output", DEFAULT_RATES["output"])),
                "cacheRead": float(
                    cost.get("cacheRead", DEFAULT_RATES["cacheRead"])),
                "cacheWrite": float(
                    cost.get("cacheWrite", DEFAULT_RATES["cacheWrite"])),
            }
    return dict(DEFAULT_RATES)


def store_pi_bin(wrapper: Path | None = None) -> Path:
    wrapper = wrapper or Path.home() / ".local/bin/pi"
    text = wrapper.read_text()
    matches = re.findall(r"/nix/store/[^ \n]+/bin/pi", text)
    if not matches:
        raise FileNotFoundError(
            "could not find store Pi binary in ~/.local/bin/pi")
    return Path(matches[-1])


def write_report(
    run_id: str,
    meta: dict[str, Any],
    rates: dict[str, float] | None = None,
    capture_path: Path | None = None,
) -> dict[str, Any]:
    rates = rates or load_rates(meta.get("model") or DEFAULT_MODEL)
    rows = load_records(capture_path or jsonl_path(), run_id)
    requests = build_requests(
        rows, rates, meta.get("model") or DEFAULT_MODEL)
    attr = attribute_costs(requests, rates)
    meta = dict(meta)
    meta["request_count"] = len(requests)
    meta["record_count"] = len(rows)
    html = render_html(meta, requests, attr, rates)
    if any(bad in html.lower() for bad in (
            "request_body", "authorization", "api-key")):
        raise RuntimeError(
            "refusing to write a report that leaked secrets")
    out = reports_dir()
    html_path = out / f"{run_id}.html"
    json_path = out / f"{run_id}.json"
    html_path.write_text(html)
    json_path.write_text(json.dumps(
        {
            "meta": meta, "rates": rates,
            "requests": requests, "attribution": attr,
        },
        indent=2, default=str,
    ))
    os.chmod(html_path, 0o600)
    os.chmod(json_path, 0o600)
    latest = out / "latest.html"
    if latest.exists() or latest.is_symlink():
        latest.unlink()
    latest.symlink_to(html_path.name)
    return {
        "html": str(html_path),
        "json": str(json_path),
        "request_count": len(requests),
        "meta": meta,
        "attribution": attr,
    }


def run_capture(
    pi_bin: Path | None = None,
    prompt_capture: Path | None = None,
    timeout: int = 900,
) -> dict[str, Any]:
    capture = prompt_capture or Path.home() / ".local/bin/prompt-capture"
    pi_bin = pi_bin or store_pi_bin()
    out = reports_dir()
    task_dir = Path(tempfile.mkdtemp(prefix="pi-mitm-task-", dir=out))
    session_dir = Path(tempfile.mkdtemp(prefix="pi-mitm-session-", dir=out))
    runner = task_dir / "run-turns.sh"
    script = f"""#!/usr/bin/env bash
set -euo pipefail
cd {task_dir.as_posix()!r}
unset PI_SESSION_FILE PI_SESSION_ID CAPTURE_PROMPTS
export PI_TELEMETRY=0
export PI_PROVIDER={DEFAULT_PROVIDER}
export PI_MODEL={DEFAULT_MODEL}
PI={pi_bin.as_posix()!r}
SESSION={session_dir.as_posix()!r}
COMMON=(--print --provider {DEFAULT_PROVIDER} --model {DEFAULT_MODEL}
        --thinking low --session-dir "$SESSION" --name mitm-token-probe)
"$PI" "${{COMMON[@]}}" {json.dumps(TURNS[0][1])}
"$PI" "${{COMMON[@]}}" --continue {json.dumps(TURNS[1][1])}
"$PI" "${{COMMON[@]}}" --continue {json.dumps(TURNS[2][1])}
"""
    runner.write_text(script)
    runner.chmod(0o700)
    env = os.environ.copy()
    for key in (
        "PI_SESSION_FILE", "PI_SESSION_ID", "CAPTURE_PROMPTS",
        "HTTP_PROXY", "HTTPS_PROXY", "http_proxy", "https_proxy",
        "SSL_CERT_FILE", "REQUESTS_CA_BUNDLE", "NODE_EXTRA_CA_CERTS",
    ):
        env.pop(key, None)
    env["PI_TELEMETRY"] = "0"
    proc = subprocess.run(
        [str(capture), "pi", "--", str(runner)],
        env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False,
    )
    (out / "nested-pi.log").write_text(proc.stdout)
    match = re.search(r"run ([0-9T]+-\d+)", proc.stdout)
    add_py = (task_dir / "add.py").exists()
    add_out = ""
    if add_py:
        try:
            add_out = subprocess.check_output(
                [sys.executable, str(task_dir / "add.py")],
                text=True, timeout=10,
            ).strip()
        except (subprocess.CalledProcessError, OSError) as exc:
            add_out = f"error: {exc}"
    return {
        "run_id": match.group(1) if match else "",
        "exit_code": proc.returncode,
        "task_result": (
            f"add.py={'yes' if add_py else 'no'} "
            f"output={add_out or 'n/a'}"
        ),
        "stdout_tail": proc.stdout[-2000:],
    }


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    rates = load_rates()
    if args[:1] == ["--from-run"] and len(args) >= 2:
        meta = {
            "run_id": args[1],
            "captured_at": datetime.now(timezone.utc).strftime(
                "%Y-%m-%d %H:%M UTC"),
            "model": DEFAULT_MODEL,
            "provider": DEFAULT_PROVIDER,
            "task_result": "rebuilt from capture",
        }
        result = write_report(args[1], meta, rates)
        print(result["html"])
        print(f"run_id={args[1]} requests={result['request_count']}")
        return 0 if result["request_count"] else 2
    if args[:1] not in ([], ["--capture"]):
        print(
            "usage: report.py [--capture] | --from-run RUN_ID",
            file=sys.stderr,
        )
        return 2
    captured = run_capture()
    if not captured["run_id"]:
        print(captured.get("stdout_tail", ""), file=sys.stderr)
        return 1
    meta = {
        "run_id": captured["run_id"],
        "captured_at": datetime.now(timezone.utc).strftime(
            "%Y-%m-%d %H:%M UTC"),
        "model": DEFAULT_MODEL,
        "provider": DEFAULT_PROVIDER,
        "task_result": captured["task_result"],
        "exit_code": captured["exit_code"],
    }
    result = write_report(captured["run_id"], meta, rates)
    print(result["html"])
    print(
        f"run_id={captured['run_id']} requests={result['request_count']} "
        f"exit={captured['exit_code']}"
    )
    print(captured["task_result"])
    return 0 if result["request_count"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
