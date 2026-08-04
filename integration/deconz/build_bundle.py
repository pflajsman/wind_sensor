#!/usr/bin/env python3
"""
Build a deCONZ DDF bundle (.ddb) for the awning wind sensor, entirely offline.

The .ddb format is a RIFF container:

    RIFF <size> DDFB <size>
        DESC <size> {json metadata}
        EXTF <size> [4B type][2B pathlen][path][2B mtimelen][mtime][4B contentlen][content]
        ... (one EXTF per file: the DDF itself + every generic item/constant it uses)

deCONZ resolves each item name in the DDF to a file in generic/items/, so every
item the DDF references must be packed as its own EXTF chunk. Inline item
descriptors in the DDF are NOT accepted by the bundler as a substitute — verified
against a real store bundle (IKEA BILRESA E2490).

Usage:  python build_bundle.py
Output: awning_wind_sensor.ddb  (next to this script)

No signature chunk is written (unsigned bundle). deCONZ accepts unsigned bundles
loaded locally; the online store additionally signs them.
"""
import struct
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))

# Fixed timestamp — the format stores an mtime string per file. We use a single
# stable value so rebuilds are reproducible (avoids Date.now-style churn).
MTIME = "2026-08-03T00:00:00.000Z"

# The main DDF, packed as file-type DDFC under a repo-style path.
DDF_FILE = ("DDFC", "diy/awning_wind_sensor.json", "awning_wind_sensor.json")

# Every generic JSON file the DDF references, packed as file-type JSON.
GENERIC_FILES = [
    "generic/constants_min.json",
    "generic/items/attr_id_item.json",
    "generic/items/attr_lastannounced_item.json",
    "generic/items/attr_lastseen_item.json",
    "generic/items/attr_manufacturername_item.json",
    "generic/items/attr_modelid_item.json",
    "generic/items/attr_name_item.json",
    "generic/items/attr_swversion_item.json",
    "generic/items/attr_type_item.json",
    "generic/items/attr_uniqueid_item.json",
    "generic/items/config_battery_item.json",
    "generic/items/state_presence_item.json",
    "generic/items/state_movement_rms_item.json",
    "generic/items/state_movement_peak_item.json",
    "generic/items/state_tilt_pitch_item.json",
    "generic/items/state_tilt_roll_item.json",
]

OUTPUT = os.path.join(HERE, "awning_wind_sensor.ddb")


def riff_chunk(tag: bytes, payload: bytes) -> bytes:
    """A RIFF chunk: 4-byte tag, 4-byte LE size, payload, pad to even length."""
    out = tag + struct.pack("<I", len(payload)) + payload
    if len(payload) % 2 == 1:
        out += b"\x00"
    return out


def extf_payload(file_type: str, repo_path: str, content: bytes) -> bytes:
    """Inner layout of an EXTF chunk (matches the store-bundle format exactly)."""
    ft = file_type.encode("ascii")
    assert len(ft) == 4, file_type
    path = repo_path.encode("utf-8")
    mtime = MTIME.encode("ascii")
    return (
        ft
        + struct.pack("<H", len(path)) + path
        + struct.pack("<H", len(mtime)) + mtime
        + struct.pack("<I", len(content)) + content
    )


def read_bytes(rel_path: str) -> bytes:
    with open(os.path.join(HERE, rel_path), "rb") as f:
        return f.read()


def build_desc() -> bytes:
    ddf = json.loads(read_bytes(DDF_FILE[2]))
    desc = {
        "version_deconz": ">2.27.0",
        "last_modified": MTIME,
        "vendor": ddf.get("manufacturername", ""),
        "product": ddf.get("product", ""),
        # deCONZ matches a device by [manufacturername, modelid].
        "device_identifiers": [[ddf["manufacturername"], ddf["modelid"]]],
    }
    return json.dumps(desc, separators=(",", ":")).encode("utf-8")


def main():
    body = b""
    body += riff_chunk(b"DESC", build_desc())

    # Main DDF
    ftype, repo_path, local = DDF_FILE
    body += riff_chunk(b"EXTF", extf_payload(ftype, repo_path, read_bytes(local)))

    # Generic dependencies
    for rel in GENERIC_FILES:
        body += riff_chunk(b"EXTF", extf_payload("JSON", rel, read_bytes(rel)))

    ddfb = riff_chunk(b"DDFB", body)
    riff = b"RIFF" + struct.pack("<I", len(ddfb)) + ddfb

    with open(OUTPUT, "wb") as f:
        f.write(riff)

    print(f"wrote {OUTPUT} ({len(riff)} bytes)")
    print(f"  DESC + 1 DDFC + {len(GENERIC_FILES)} JSON files")


if __name__ == "__main__":
    main()
