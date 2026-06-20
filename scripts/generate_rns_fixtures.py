#!/usr/bin/env python3
"""Compatibility wrapper for the canonical Reticulum fixture generator."""

from __future__ import annotations

import runpy
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "tools" / "rns_fixtures" / "generate.py"


if __name__ == "__main__":
    runpy.run_path(str(GENERATOR), run_name="__main__")
