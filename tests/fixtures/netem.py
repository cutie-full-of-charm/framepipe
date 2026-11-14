import os
import platform
import shutil
import subprocess
from contextlib import contextmanager
from typing import Tuple

INTERFACE = "lo"


def netem_available() -> Tuple[bool, str]:

    if platform.system() != "Linux":
        return False, f"OS is not Linux, OS: {platform.system()}"
    if shutil.which("tc") is None:
        return False, "tc binary not found"
    if hasattr(os, "geteuid") and os.geteuid() != 0:
        return False, "Need root/CAP_NET_ADMIN"
    return True, ""


def _tc(*args):
    subprocess.run(
        args=["sudo", "tc", *args],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )


def _clear_tc():
    subprocess.run(
        args=["sudo", "tc", "qdisc", "del", "dev", INTERFACE, "root"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


@contextmanager
def network_corrupt(
    delay_ms=0.0,
    jitter_ms=0.0,
    loss_packets_percent=0.0,
    reorder_packets_percent=0.0,
    duplicate_packets_percent=0.0,
):
    ok, reason = netem_available()
    if not ok:
        raise RuntimeError(f"Netem unavailable: {reason}")

    cmd = ["qdisc", "add", "dev", INTERFACE, "root", "netem"]
    if delay_ms > 0:
        cmd += ["delay", f"{delay_ms}ms"]
        if jitter_ms > 0:
            cmd += [f"{jitter_ms}ms"]

    if loss_packets_percent > 0:
        cmd += ["loss", f"{loss_packets_percent}%"]

    if reorder_packets_percent > 0:
        cmd += ["reorder", f"{reorder_packets_percent}%"]

    if duplicate_packets_percent > 0:
        cmd += ["duplicate", f"{duplicate_packets_percent}%"]

    _clear_tc()
    _tc(*cmd)
    try:
        yield
    finally:
        _clear_tc()
