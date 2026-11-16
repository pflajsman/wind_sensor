#!/usr/bin/env python3
"""
Build a deCONZ DDF bundle (.ddb) for awning-protection, entirely offline.

Same RIFF/DDFB container format as build_bundle.py (verified against a real store
bundle), but for the minimal single-endpoint awning_protection.json — so it packs
only the generic items that DDF actually references (the 9 attr_* items plus
state/presence, config/battery and config/sensitivity). No movement/tilt items.

    RIFF <size> DDFB <size>
        DESC <size> {json metadata}
        EXTF <size> [4B type][2B pathlen][path][2B mtimelen][mtime][4B contentlen][content]
        ... (one EXTF per file: the DDF itself + every generic item/constant it uses)

Usage:  python build_bundle_protection.py
Output: awning_protection.ddb  (next to this script)

Unsigned bundle. deCONZ accepts unsigned bundles loaded locally.
"""
import struct
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))

# Fixed timestamp so rebuilds are reproducible (the format stores an mtime string
# per file; a stable value avoids Date.now-style churn).
MTIME = "2026-08-04T00:00:00.000Z"

# The main DDF, packed as file-type DDFC under a repo-style path.
DDF_FILE = ("DDFC", "diy/awning_protection.json", "awning_protection.json")

# Every generic JSON file the DDF references, packed as file-type JSON. The minimal
# DDF uses only these — the 9 standard attr items plus presence + battery.
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
    "generic/items/config_on_item.json",
    "generic/items/config_reachable_item.json",
    "generic/items/config_sensitivity_item.json",
    "generic/items/state_lastupdated_item.json",
    "generic/items/state_presence_item.json",
]

OUTPUT = os.path.join(HERE, "awning_protection.ddb")


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
