from __future__ import annotations

import datetime as dt
import json
import tempfile
import unittest
from pathlib import Path

from tools.claude_usage.__main__ import (
    SnapshotStore,
    UsageError,
    build_snapshot,
    read_access_token,
    refresh_once,
)

USAGE = {
    "five_hour": {"utilization": 13.0, "resets_at": "2026-10-28T00:30:00.020993+00:00"},
    "seven_day": {"utilization": 15, "resets_at": "2026-10-31T22:00:00.021014+00:00"},
    "extra_usage": {"is_enabled": False},
}


def write_credentials(directory: str, oauth: object) -> Path:
    path = Path(directory) / ".credentials.json"
    path.write_text(json.dumps({"claudeAiOauth": oauth}), encoding="utf-8")
    return path


class ClaudeUsageTests(unittest.TestCase):
    def test_snapshot_projects_utilization_and_reset_epoch_only(self) -> None:
        snapshot = build_snapshot(USAGE)
        reset = dt.datetime(2026, 10, 28, 0, 30, tzinfo=dt.timezone.utc)
        self.assertEqual(
            snapshot["five_hour"],
            {"utilization": 13.0, "resets_at_epoch": int(reset.timestamp())},
        )
        self.assertEqual(snapshot["seven_day"]["utilization"], 15.0)
        self.assertNotIn("extra_usage", snapshot)

    def test_idle_window_has_null_reset(self) -> None:
        snapshot = build_snapshot({**USAGE, "five_hour": {"utilization": 0, "resets_at": None}})
        self.assertIsNone(snapshot["five_hour"]["resets_at_epoch"])

    def test_missing_window_is_rejected(self) -> None:
        with self.assertRaises(UsageError):
            build_snapshot({"five_hour": USAGE["five_hour"]})

    def test_expired_token_is_rejected_without_echoing_it(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = write_credentials(directory, {"accessToken": "secret-token", "expiresAt": 1000})
            with self.assertRaises(UsageError) as raised:
                read_access_token(path, now_ms=2000)
            self.assertNotIn("secret-token", str(raised.exception))
            self.assertEqual(read_access_token(path, now_ms=500), "secret-token")

    def test_failed_refresh_drops_previous_values(self) -> None:
        store = SnapshotStore()
        with tempfile.TemporaryDirectory() as directory:
            path = write_credentials(directory, {"accessToken": "t", "expiresAt": 2**62})
            refresh_once(store, path=path, timeout_seconds=1, fetch=lambda token, timeout: USAGE)
            self.assertEqual(store.response()["five_hour"]["utilization"], 13.0)

            def fail(token: str, timeout: float) -> dict[str, object]:
                raise UsageError("Claude usage endpoint returned HTTP 401")

            refresh_once(store, path=path, timeout_seconds=1, fetch=fail)
        document = store.response()
        self.assertNotIn("five_hour", document)
        self.assertEqual(document["collector"]["status"], "error")
        self.assertIsNotNone(document["collector"]["last_success_at"])


if __name__ == "__main__":
    unittest.main()
