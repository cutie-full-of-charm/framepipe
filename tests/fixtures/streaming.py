import json
import socket
import time
import urllib

RTSP_HOST = "127.0.0.1"
RTSP_PORT = 8554
RTSP_LINK = f"rtsp://{RTSP_HOST}:{RTSP_PORT}"

DEAD_PORT = 8624
DEAD_LINK = f"rtsp://{RTSP_HOST}:{DEAD_PORT}"


def mediamtx_reachable(timeout=1):
    try:
        with socket.create_connection((RTSP_HOST, RTSP_PORT), timeout=timeout):
            return True, ""
    except Exception as e:
        return False, e


def wait_stream_ready(stream, timeout=10.0):
    deadline = time.monotonic() + timeout
    url = "http://127.0.0.1:9997/v3/paths/list"
    last_err = None
    while time.monotonic() < deadline:
        data = json.load(urllib.request.urlopen(url, timeout=0.5))  # pyright: ignore
        for p in data.get("items", []):
            if p.get("name") == stream and p.get("ready"):
                return

        time.sleep(0.05)
    raise TimeoutError(f"Stream({stream}) not ready in {timeout}s")


def rtsp_url(stream):
    return f"{RTSP_LINK}/{stream}"


def dead_rtsp_url(stream="none"):
    return f"{DEAD_LINK}/{stream}"
