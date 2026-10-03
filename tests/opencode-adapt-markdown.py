#!/usr/bin/env python3
import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "home/config/opencode/adapt-markdown.py"
spec = importlib.util.spec_from_file_location("adapt_markdown", SCRIPT)
adapt_markdown = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapt_markdown)


class AdaptMarkdownTest(unittest.TestCase):
    def test_agent_drops_pi_keys_and_keeps_skills_in_body(self):
        text = """---
name: builder
description: Implement an agreed task
tools: read, bash
skills: [verification-before-completion]
model: xai/grok-4.7
thinking: medium
---
Do the work.
"""
        result = adapt_markdown.adapt(text, "agent")
        self.assertIn("mode: subagent", result)
        self.assertIn("model: xai/grok-4.7", result)
        self.assertNotIn("thinking:", result)
        self.assertNotIn("tools:", result)
        self.assertIn("verification-before-completion", result)
        self.assertIn(
            "Load these skills when the task reaches their trigger: "
            "verification-before-completion.",
            result,
        )

    def test_command_replaces_pi_argument_token(self):
        text = """---
description: Design something
argument-hint: "<brief>"
---
Use $@
"""
        result = adapt_markdown.adapt(text, "command")
        self.assertNotIn("argument-hint", result)
        self.assertIn("$ARGUMENTS", result)
        self.assertNotIn("$@", result)


if __name__ == "__main__":
    unittest.main()
