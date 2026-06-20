#!/usr/bin/env python3
"""Generate deterministic Reticulum compatibility fixtures for MeshPayV2.

The manifest is stable by default so CI and C fixture tests can lock the wire
vectors. When the official Reticulum Python package is installed,
--verify-reticulum checks protocol constants against that implementation
without changing the generated file.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


SCHEMA = "meshpay-rns-fixtures-v1"
SOURCE = "local-port-c-fixtures"

RETICULUM_PACKET_CONTEXTS = {
    "RESOURCE": 0x01,
    "RESOURCE_ADV": 0x02,
    "RESOURCE_REQ": 0x03,
    "RESOURCE_HMU": 0x04,
    "RESOURCE_PRF": 0x05,
    "RESOURCE_ICL": 0x06,
    "RESOURCE_RCL": 0x07,
    "CACHE_REQUEST": 0x08,
    "REQUEST": 0x09,
    "RESPONSE": 0x0A,
    "PATH_RESPONSE": 0x0B,
    "COMMAND": 0x0C,
    "COMMAND_STATUS": 0x0D,
    "CHANNEL": 0x0E,
    "KEEPALIVE": 0xFA,
    "LINKIDENTIFY": 0xFB,
    "LINKCLOSE": 0xFC,
    "LINKPROOF": 0xFD,
    "LRRTT": 0xFE,
    "LRPROOF": 0xFF,
}

LOCAL_VECTORS = [
    {
        "name": "schema-canary-v1",
        "kind": "schema_canary",
        "hex": "4d50524e53465801",
    },
    {
        "name": "meshpay-wallet-name-hash",
        "kind": "destination_name_hash",
        "full_name": "meshpay.wallet",
        "hex": "049d9046c74de46b50a3",
    },
    {
        "name": "meshpay-wallet-destination-hash",
        "kind": "destination_hash",
        "full_name": "meshpay.wallet",
        "identity_private_hex": (
            "77076d0a7318a57d3c16c17251b26645"
            "df4c2f87ebc0992ab177fba51db92c2a"
            "9d61b19deffd5a60ba844af492ec2cc4"
            "4449c5697b326919703bac031cae3d55"
        ),
        "hex": "8b61de206a0ebcae6542580479d843c4",
    },
    {
        "name": "data-packet-type1-raw",
        "kind": "packet_raw",
        "hex": "0007101112131415161718191a1b1c1d1e1fab6869",
    },
    {
        "name": "announce-meshpay-wallet-alice",
        "kind": "announce_raw",
        "random_hash_hex": "a0a1a2a3a4a5a6a7a8a9",
        "app_data_hex": "416c696365",
        "hex": (
            "8520f0098930a754748b7ddcb43ef75a"
            "0dbf3a0d26381af4eba4a98eaa9b4e6a"
            "700e2ce7c4b674427eab27ba820bcf6f"
            "0faebe68e09fe8564292114e41dc6a41"
            "049d9046c74de46b50a3a0a1a2a3a4a5"
            "a6a7a8a956f57401e5878bcdd07c2b87"
            "138188ac573715cb637b64477f433033"
            "ebcce39f10635e0d33c00521643d2a46"
            "79c9bbf97cc794059da2ab110d1eef31"
            "23075002416c696365"
        ),
    },
    {
        "name": "encrypted-token-single",
        "kind": "encrypted_token",
        "plaintext_hex": "6d657368706179207265746963756c756d20656e637279707465642064617461",
        "hex": (
            "de9edb7d7b7dc1b4d35b61c2ece43537"
            "3f8343c85b78674dadfc7e146f882b4f"
            "000102030405060708090a0b0c0d0e0f"
            "4ac098edf465a84a53164515ddc1d029"
            "0c93981ce4d4654cd432b2a0026e0302"
            "b64f497ee1afdf078d906fbe616e86e2"
            "2f656e98d479532c29aad69e185f7ff7"
            "0dcb ded7981af8d2219a39fc2a912678".replace(" ", "")
        ),
    },
]


def load_reticulum() -> Any:
    try:
        import RNS  # type: ignore
    except ModuleNotFoundError as exc:
        raise RuntimeError(
            "Reticulum Python package is not installed. Install the official "
            "package before generating compatibility vectors."
        ) from exc
    return RNS


def build_schema_only_manifest() -> dict[str, Any]:
    return {
        "schema": SCHEMA,
        "source": SOURCE,
        "status": "schema-only",
        "vectors": [],
    }


def build_local_port_manifest() -> dict[str, Any]:
    return {
        "schema": SCHEMA,
        "source": SOURCE,
        "status": "local-port-generated",
        "vectors": LOCAL_VECTORS,
    }


def verify_reticulum_constants() -> None:
    rns = load_reticulum()
    packet = getattr(rns, "Packet", None)
    if packet is None:
        raise RuntimeError("Reticulum package does not expose RNS.Packet")

    mismatches = []
    for name, expected in RETICULUM_PACKET_CONTEXTS.items():
        actual = getattr(packet, name, None)
        if actual != expected:
            mismatches.append(f"{name}: expected 0x{expected:02x}, got {actual!r}")
    if mismatches:
        joined = "\n  ".join(mismatches)
        raise RuntimeError(f"Reticulum packet context mismatch:\n  {joined}")


def stable_json(data: dict[str, Any]) -> str:
    return json.dumps(data, indent=2, ensure_ascii=True) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--out",
        "--output",
        dest="out",
        type=Path,
        default=Path("fixtures/rns/manifest.json"),
        help="Output manifest path.",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Fail if the output file differs from generated content.",
    )
    parser.add_argument(
        "--allow-schema-only",
        action="store_true",
        help="Write only the fixture schema manifest if Reticulum is unavailable.",
    )
    parser.add_argument(
        "--verify-reticulum",
        action="store_true",
        help="Verify Reticulum Python packet constants if the package is installed.",
    )
    args = parser.parse_args()

    if args.allow_schema_only:
        data = build_schema_only_manifest()
    else:
        data = build_local_port_manifest()

    if args.verify_reticulum:
        verify_reticulum_constants()

    rendered = stable_json(data)
    if args.check:
        try:
            current = args.out.read_text(encoding="utf-8")
        except FileNotFoundError:
            print(f"missing fixture manifest: {args.out}")
            return 1
        if current != rendered:
            print(f"fixture manifest is not up to date: {args.out}")
            return 1
        print(f"fixture manifest OK: {args.out}")
        return 0

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(rendered, encoding="utf-8")
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
