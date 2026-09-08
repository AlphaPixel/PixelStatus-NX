"""Atomic, privacy-preserving status output for unattended BLE bridging."""

from __future__ import annotations

import datetime as dt
import json
import logging
import os
import time
from pathlib import Path
from typing import Any

LOGGER = logging.getLogger("pixelstatus.mi_ble")


class BridgeStatusFile:
    def __init__(self, path: str | None) -> None:
        self._path = Path(path).resolve() if path else None
        self._temporary = (
            self._path.with_name(f"{self._path.name}.{os.getpid()}.next")
            if self._path is not None
            else None
        )

    def update(self, status: str, **fields: Any) -> bool:
        if self._path is None:
            return True
        document = {
            "schema_version": 1,
            "status": status,
            "updated_at": dt.datetime.now(dt.timezone.utc).isoformat().replace(
                "+00:00", "Z"
            ),
            **fields,
        }
        assert self._temporary is not None
        try:
            self._path.parent.mkdir(parents=True, exist_ok=True)
            self._temporary.write_text(
                json.dumps(document, separators=(",", ":")),
                encoding="utf-8",
            )
            for attempt in range(3):
                try:
                    os.replace(self._temporary, self._path)
                    return True
                except OSError:
                    if attempt == 2:
                        raise
                    time.sleep(0.02 * (attempt + 1))
        except OSError as error:
            LOGGER.warning(
                "Unable to update MI heartbeat %s: %s",
                self._path,
                error,
            )
            try:
                self._temporary.unlink(missing_ok=True)
            except OSError:
                pass
            return False
