"""Atomic, privacy-preserving status output for unattended BLE bridging."""

from __future__ import annotations

import datetime as dt
import json
import os
from pathlib import Path
from typing import Any


class BridgeStatusFile:
    def __init__(self, path: str | None) -> None:
        self._path = Path(path).resolve() if path else None

    def update(self, status: str, **fields: Any) -> None:
        if self._path is None:
            return
        document = {
            "schema_version": 1,
            "status": status,
            "updated_at": dt.datetime.now(dt.timezone.utc).isoformat().replace(
                "+00:00", "Z"
            ),
            **fields,
        }
        self._path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self._path.with_name(f"{self._path.name}.next")
        temporary.write_text(
            json.dumps(document, separators=(",", ":")),
            encoding="utf-8",
        )
        os.replace(temporary, self._path)
