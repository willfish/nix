"""Offline checks for the agents usage records and bar status."""

import importlib.util
import json
import os
from datetime import datetime
from pathlib import Path
import tempfile
import unittest


MODULE = Path(
    os.environ.get(
        "AGENT_USAGE_LIB",
        Path(__file__).resolve().parents[1]
        / "home/config/hyprland/agents/usage_lib.py",
    )
)


class UsageTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location("usage_lib", MODULE)
        self.lib = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.lib)

    def test_grok_session_counts_the_largest_snapshot_once(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            session = root / "demo" / "session-a"
            session.mkdir(parents=True)
            events = [
                {
                    "timestamp": 1_780_000_000,
                    "params": {
                        "sessionId": "session-a",
                        "update": {
                            "sessionUpdate": "turn_completed",
                            "usage": {
                                "totalTokens": 100,
                                "numTurns": 1,
                                "inputTokens": 80,
                                "outputTokens": 20,
                                "cachedReadTokens": 10,
                                "modelUsage": {
                                    "grok-4.5": {
                                        "inputTokens": 80,
                                        "outputTokens": 20,
                                        "cachedReadTokens": 10,
                                    }
                                },
                            },
                        },
                    },
                },
                {
                    "timestamp": 1_780_000_100,
                    "params": {
                        "sessionId": "session-a",
                        "update": {
                            "sessionUpdate": "turn_completed",
                            "usage": {
                                "totalTokens": 250,
                                "numTurns": 2,
                                "inputTokens": 200,
                                "outputTokens": 50,
                                "cachedReadTokens": 40,
                                "modelUsage": {
                                    "grok-4.5": {
                                        "inputTokens": 200,
                                        "outputTokens": 50,
                                        "cachedReadTokens": 40,
                                    }
                                },
                            },
                        },
                    },
                },
                {
                    "timestamp": 1_780_000_050,
                    "params": {
                        "sessionId": "session-a",
                        "update": {
                            "sessionUpdate": "tool_call",
                            "usage": {"totalTokens": 9999},
                        },
                    },
                },
            ]
            (session / "updates.jsonl").write_text(
                "\n".join(json.dumps(event) for event in events) + "\n"
            )
            stats = self.lib.scan_grok_sessions(
                root, datetime.fromtimestamp(1_780_000_100)
            )
        self.assertEqual(stats["totalSessions"], 1)
        self.assertEqual(stats["totalPrompts"], 2)
        self.assertEqual(stats["modelUsage"]["grok-4.5"]["inputTokens"], 160)

    def test_grok_billing_reads_weekly_pool_and_build_share(self):
        limits = self.lib.parse_grok_billing(
            {
                "config": {
                    "creditUsagePercent": 32,
                    "currentPeriod": {
                        "type": "USAGE_PERIOD_TYPE_WEEKLY",
                        "end": "2026-10-06T00:00:00Z",
                    },
                    "productUsage": [
                        {"product": "GrokBuild", "usagePercent": 29}
                    ],
                }
            }
        )
        self.assertEqual(limits[0]["percent"], 0.32)
        self.assertEqual(limits[0]["title"], "Weekly")
        self.assertEqual(limits[1]["percent"], 0.29)
        self.assertEqual(limits[1]["title"], "Grok Build")

    def test_grok_billing_reads_one_point_zero_as_one_percent(self):
        limits = self.lib.parse_grok_billing(
            {
                "config": {
                    "creditUsagePercent": 1.0,
                    "currentPeriod": {
                        "type": "USAGE_PERIOD_TYPE_WEEKLY",
                        "end": "2026-10-08T20:25:49+00:00",
                    },
                    "productUsage": [
                        {"product": "GrokBuild", "usagePercent": 1.0},
                        {"product": "GrokChat"},
                    ],
                }
            }
        )
        self.assertEqual(limits[0]["percent"], 0.01)
        self.assertEqual(limits[1]["percent"], 0.01)
        self.assertEqual(
            self.lib.parse_grok_billing(
                {
                    "config": {
                        "creditUsagePercent": 2.0,
                        "currentPeriod": {
                            "end": "2026-10-08T20:25:49+00:00",
                        },
                        "productUsage": [
                            {"product": "GrokBuild", "usagePercent": 2.0}
                        ],
                    }
                }
            )[0]["percent"],
            0.02,
        )

    def test_grok_billing_reads_omitted_percent_as_unused_weekly_pool(self):
        limits = self.lib.parse_grok_billing(
            {
                "config": {
                    "currentPeriod": {
                        "type": "USAGE_PERIOD_TYPE_WEEKLY",
                        "start": "2026-10-01T20:25:49+00:00",
                        "end": "2026-10-08T20:25:49+00:00",
                    },
                    "onDemandCap": {"val": 0},
                    "onDemandUsed": {"val": 0},
                    "isUnifiedBillingUser": True,
                    "prepaidBalance": {"val": 0},
                    "billingPeriodEnd": "2026-10-08T20:25:49+00:00",
                }
            }
        )
        self.assertEqual(len(limits), 1)
        self.assertEqual(limits[0]["title"], "Weekly")
        self.assertEqual(limits[0]["percent"], 0.0)
        self.assertEqual(limits[0]["resetsAt"], "2026-10-08T20:25:49+00:00")

    def test_grok_billing_does_not_invent_usage_without_a_period(self):
        self.assertEqual(
            self.lib.parse_grok_billing(
                {"config": {"isUnifiedBillingUser": True}}
            ),
            [],
        )
        self.assertEqual(
            self.lib.parse_grok_billing(
                {
                    "config": {
                        "currentPeriod": {
                            "type": "USAGE_PERIOD_TYPE_WEEKLY",
                            "end": "2026-10-08T20:25:49+00:00",
                        },
                        "creditUsagePercent": "nope",
                        "isUnifiedBillingUser": True,
                    }
                }
            ),
            [],
        )

    def test_refresh_replaces_tokens_and_keeps_profile_fields(self):
        merged = self.lib.merge_refreshed_login(
            {
                "key": "old",
                "refresh_token": "old-refresh",
                "email": "kept@example",
            },
            {
                "access_token": "new",
                "refresh_token": "new-refresh",
                "expires_in": 3600,
            },
            datetime.fromisoformat("2026-09-29T12:00:00+00:00"),
        )
        self.assertEqual(merged["key"], "new")
        self.assertEqual(merged["refresh_token"], "new-refresh")
        self.assertEqual(merged["email"], "kept@example")
        self.assertTrue(merged["expires_at"].startswith("2026-09-29T13:00:00"))

    def test_opencode_ignores_other_providers_and_reads_go_windows(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw) / "storage" / "message" / "ses"
            root.mkdir(parents=True)
            (root / "go.json").write_text(
                json.dumps(
                    {
                        "role": "assistant",
                        "providerID": "opencode",
                        "modelID": "deepseek-v4-flash",
                        "sessionID": "ses",
                        "time": {"created": 1_780_000_000_000},
                        "tokens": {
                            "input": 10,
                            "output": 4,
                            "reasoning": 1,
                            "cache": {"read": 2, "write": 3},
                        },
                    }
                )
            )
            (root / "router.json").write_text(
                json.dumps(
                    {
                        "role": "assistant",
                        "providerID": "openrouter",
                        "modelID": "other",
                        "sessionID": "other",
                        "tokens": {
                            "input": 100,
                            "output": 100,
                            "reasoning": 0,
                            "cache": {},
                        },
                    }
                )
            )
            stats = self.lib.scan_opencode_messages(
                Path(raw) / "storage", datetime.fromtimestamp(1_780_000_000)
            )
        self.assertEqual(stats["totalPrompts"], 1)
        self.assertEqual(
            stats["modelUsage"]["deepseek-v4-flash"]["outputTokens"], 5
        )
        limits, plan = self.lib.parse_opencode_usage(
            {
                "plan": "go plus",
                "usage": {
                    "rolling": {
                        "percent": 4,
                        "resetsAt": "2026-09-29T18:00:00Z",
                    },
                    "weekly": {
                        "percent": 8,
                        "resetsAt": "2026-10-06T00:00:00Z",
                    },
                    "monthly": {
                        "percent": 2,
                        "resetsAt": "2026-10-29T00:00:00Z",
                    },
                },
            }
        )
        self.assertEqual(plan, "Go Plus")
        self.assertEqual(
            [item["title"] for item in limits], ["Session", "Weekly", "Monthly"]
        )
        self.assertEqual(limits[0]["percent"], 0.04)

    def test_bar_status_lists_subscriptions_and_alarms(self):
        quiet = self.lib.waybar_status(
            [
                {
                    "id": "codex",
                    "name": "Codex",
                    "tierLabel": "Max 20x",
                    "limits": [{"title": "Session", "percent": 0.4}],
                },
                {
                    "id": "grok",
                    "name": "Grok",
                    "tierLabel": "SuperGrok",
                    "limits": [{"title": "Weekly", "percent": 0.91}],
                },
            ]
        )
        self.assertEqual(quiet["class"], "alarm")
        self.assertIn("Codex · Max 20x · Session 40%", quiet["tooltip"])
        self.assertIn("Grok · SuperGrok · Weekly 91%", quiet["tooltip"])
        self.assertNotIn("token", quiet["tooltip"].lower())

    def test_pi_sessions_and_codex_plan_window(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            (root / "session.jsonl").write_text(
                "\n".join(
                    [
                        json.dumps(
                            {
                                "id": "turn-1",
                                "timestamp": 1_780_000_000_000,
                                "message": {
                                    "role": "assistant",
                                    "provider": "openai-codex",
                                    "model": "gpt-6-astra",
                                    "timestamp": 1_780_000_000_000,
                                    "usage": {
                                        "input": 10,
                                        "output": 2,
                                        "cacheRead": 1,
                                        "cacheWrite": 0,
                                        "reasoning": 3,
                                    },
                                    "content": "hidden",
                                },
                            }
                        ),
                        json.dumps(
                            {
                                "id": "turn-2",
                                "message": {
                                    "role": "assistant",
                                    "provider": "xai",
                                    "model": "grok-4.7",
                                    "usage": {
                                        "input": 99,
                                        "output": 1,
                                        "cacheRead": 0,
                                        "cacheWrite": 0,
                                    },
                                },
                            }
                        ),
                    ]
                )
                + "\n"
            )
            codex = self.lib.scan_pi_sessions(
                root, "openai-codex", datetime.fromtimestamp(1_780_000_000)
            )
            grok = self.lib.scan_pi_sessions(
                root, "xai", datetime.fromtimestamp(1_780_000_000)
            )
        self.assertEqual(codex["totalPrompts"], 1)
        self.assertEqual(codex["totalSessions"], 1)
        self.assertEqual(codex["modelUsage"]["gpt-6-astra"]["outputTokens"], 5)
        self.assertEqual(grok["totalPrompts"], 1)
        self.assertNotIn("hidden", json.dumps(codex))
        limits, plan, status = self.lib.parse_codex_wham(
            {
                "plan_type": "pro",
                "rate_limit": {
                    "allowed": True,
                    "primary_window": {
                        "used_percent": 99,
                        "limit_window_seconds": 604800,
                        "reset_after_seconds": 3600,
                    },
                    "secondary_window": {},
                },
            },
            datetime.fromisoformat("2026-09-29T12:00:00+00:00"),
        )
        self.assertEqual(plan, "Pro")
        self.assertEqual(status, "")
        self.assertEqual(limits[0]["title"], "Weekly")
        self.assertEqual(limits[0]["percent"], 0.99)
        self.assertEqual(limits[0]["resetsAt"], "2026-09-29T13:00:00+00:00")
        stamped, _, _ = self.lib.parse_codex_wham(
            {
                "plan_type": "pro",
                "rate_limit": {
                    "primary_window": {
                        "used_percent": 99,
                        "limit_window_seconds": 604800,
                        "reset_at": 1791050635,
                    }
                },
            }
        )
        self.assertTrue(stamped[0]["resetsAt"].startswith("2026-10-03T"))
        self.assertNotIn("1791050635", stamped[0]["resetsAt"])

    def test_pi_refresh_keeps_account_id(self):
        merged = self.lib.merge_pi_oauth(
            {
                "type": "oauth",
                "access": "old",
                "refresh": "old",
                "accountId": "acct",
                "expires": 1,
            },
            {
                "access_token": "new",
                "refresh_token": "newer",
                "expires_in": 3600,
            },
            1_780_000_000_000,
        )
        self.assertEqual(merged["access"], "new")
        self.assertEqual(merged["accountId"], "acct")
        self.assertGreater(merged["expires"], 1_780_000_000_000)

    def test_opencode_key_comes_from_the_go_entry(self):
        self.assertEqual(
            self.lib.extract_opencode_key(
                {"opencode-go": {"type": "api", "key": "secret"}}
            ),
            "secret",
        )
        self.assertEqual(self.lib.extract_opencode_key({}), "")


if __name__ == "__main__":
    unittest.main()
