"""Credential-safe UniFi Network monitoring helpers for the desktop host."""

from .client import (
    CertificatePinError,
    PinnedHttpsJsonClient,
    UnifiError,
    build_snapshot,
    resolve_named_secret,
)

__all__ = [
    "CertificatePinError",
    "PinnedHttpsJsonClient",
    "UnifiError",
    "build_snapshot",
    "resolve_named_secret",
]
