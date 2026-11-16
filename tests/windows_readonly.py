"""Assertions for Wine's unsupported ACL/Copy attribute translation."""
from pathlib import Path
from drag_out_smoke import wait_for


def cache_roots(prefix):
    return set(Path(prefix).glob("drive_c/users/*/AppData/Local/Temp/Waybridge-*"))


def assert_refused(prefix, previous, log, probe, basename):
    wait_for(lambda: "Read-only delivery unsupported" in Path(log.name).read_text())
    # Wait for cancellation after the explicit error. Do not
    # accept an invisible but writable cache, or reuse a stale successful one.
    wait_for(lambda: cache_roots(prefix) <= previous)
    result = probe("files", check=False)
    assert basename not in result.stdout, "unsupported cache escaped through CF_HDROP"
