import json
import math
import re
import struct
from base64 import b64encode


IMEI_RE = re.compile(r"^[0-9]{10,20}$")


def is_valid_imei(value):
    return isinstance(value, str) and IMEI_RE.fullmatch(value) is not None


def _compact_number(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    try:
        binary32_value = struct.unpack("f", struct.pack("f", value))[0]
    except (OverflowError, struct.error):
        return None
    if not math.isfinite(binary32_value):
        return None
    if isinstance(value, float) and value.is_integer():
        return int(value)
    return value


def normalize_config_payload(config):
    if isinstance(config, str):
        if config.strip() in ("", "undefined", "null"):
            return None
        try:
            config = json.loads(config)
        except json.JSONDecodeError:
            return None
    if not isinstance(config, list):
        return None

    limits = {}
    for item in config:
        if not isinstance(item, dict):
            continue
        if item.get("sensorCtgyType") not in (None, "TMP"):
            continue
        sensor_id = item.get("userSensorId")
        if (isinstance(sensor_id, bool) or sensor_id not in (1, 2) or
                sensor_id in limits):
            return None
        upper = _compact_number(item.get("setTmpUpLimit"))
        if upper is None:
            return None
        limits[sensor_id] = upper
    if set(limits) != {1, 2}:
        return None
    return [limits[1], limits[2]]


def build_config_publish(clientid, config):
    if not is_valid_imei(clientid):
        return None

    payload_obj = normalize_config_payload(config)
    if payload_obj is None:
        return None

    payload = json.dumps(payload_obj, ensure_ascii=False, separators=(",", ":"))
    if len(payload.encode("ascii")) > 80:
        return None

    return {
        "topic": f"devices/{clientid}/config",
        "qos": 1,
        "retain": False,
        "payload": payload,
    }


def build_emqx_auth_header(auth_header=None, api_key=None, api_secret=None):
    if auth_header:
        return auth_header
    if api_key and api_secret:
        token = b64encode(f"{api_key}:{api_secret}".encode("utf-8")).decode("ascii")
        return f"Basic {token}"
    return None


def publish_config_to_emqx(emqx_api, auth_header, publish_body, timeout=5):
    if not emqx_api or not auth_header or publish_body is None:
        return False, "missing emqx config"

    import requests

    response = requests.post(
        f"{emqx_api.rstrip('/')}/publish",
        headers={
            "Authorization": auth_header.replace("Authorization:", "", 1).strip(),
            "Content-Type": "application/json",
        },
        json=publish_body,
        timeout=timeout,
    )
    if 200 <= response.status_code < 300:
        return True, ""
    return False, f"emqx publish status {response.status_code}"
