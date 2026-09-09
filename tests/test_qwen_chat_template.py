"""Run with:
nix-shell -p 'python3.withPackages (ps: [ ps.jinja2 ])' \\
  --run 'python3 tests/test_qwen_chat_template.py'
"""

import pathlib
import unittest

try:
    import jinja2
except ImportError:
    jinja2 = None


TEMPLATE = (
    pathlib.Path(__file__).resolve().parents[1]
    / "home/config/local-llm/qwen3.8-chat-template.jinja"
)


def reject(message):
    raise ValueError(message)


@unittest.skipIf(jinja2 is None, "Run in the documented Nix Jinja2 environment")
class QwenChatTemplateTest(unittest.TestCase):
    def render(self, messages, **kwargs):
        env = jinja2.Environment()
        env.globals["raise_exception"] = reject
        return env.from_string(TEMPLATE.read_text()).render(
            messages=messages,
            add_generation_prompt=True,
            enable_thinking=False,
            **kwargs,
        )

    def test_leading_instructions_are_merged(self):
        text = self.render(
            [
                {"role": "system", "content": "First"},
                {"role": "developer", "content": "Second"},
                {"role": "user", "content": "Question"},
            ]
        )
        self.assertTrue(
            text.startswith("<|im_start|>system\nFirst\nSecond<|im_end|>\n")
        )
        self.assertNotIn("<system-reminder>", text)

    def test_late_instructions_preserve_order_and_content(self):
        for role in ("system", "developer"):
            with self.subTest(role=role):
                text = self.render(
                    [
                        {"role": "user", "content": "Question"},
                        {
                            "role": role,
                            "content": [{"type": "text", "text": "Reminder"}],
                        },
                    ]
                )
                self.assertIn(
                    "<|im_start|>user\nQuestion<|im_end|>\n"
                    "<|im_start|>user\n<system-reminder>\nReminder\n"
                    "</system-reminder><|im_end|>\n<|im_start|>assistant\n",
                    text,
                )

    def test_tools_and_thinking_are_preserved(self):
        text = self.render(
            [
                {"role": "user", "content": "Question"},
                {
                    "role": "assistant",
                    "content": "",
                    "reasoning_content": "Consider",
                    "tool_calls": [
                        {
                            "function": {
                                "name": "lookup",
                                "arguments": {"key": "value"},
                            }
                        }
                    ],
                },
                {"role": "tool", "content": "Result"},
                {"role": "system", "content": "Continue"},
            ],
            tools=[{"type": "function", "function": {"name": "lookup"}}],
            preserve_thinking=True,
        )
        for expected in (
            "<tools>",
            "<think>\nConsider\n</think>",
            "<function=lookup>",
            "<parameter=key>\nvalue\n</parameter>",
            "<tool_response>\nResult\n</tool_response><|im_end|>",
            "<system-reminder>\nContinue\n</system-reminder>",
        ):
            self.assertIn(expected, text)

    def test_system_images_still_rejected(self):
        with self.assertRaisesRegex(
            ValueError, "System message cannot contain images"
        ):
            self.render(
                [
                    {"role": "user", "content": "Question"},
                    {
                        "role": "system",
                        "content": [{"type": "image", "image": "unused"}],
                    },
                ]
            )


if __name__ == "__main__":
    unittest.main()
