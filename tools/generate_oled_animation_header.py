#!/usr/bin/env python3
"""Generate OLED animation asset header from assets/animations/manifest.yaml."""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml

try:
    from PIL import Image
except ImportError:  # pragma: no cover - optional dependency
    Image = None


def c_str(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def as_int(value: Any, field: str) -> int:
    if not isinstance(value, int):
        raise ValueError(f"{field} must be an integer")
    return value


def as_bool(value: Any, field: str) -> bool:
    if not isinstance(value, bool):
        raise ValueError(f"{field} must be a bool")
    return value


def sanitize_symbol(value: str) -> str:
    value = re.sub(r"[^a-zA-Z0-9_]", "_", value.strip())
    value = re.sub(r"_+", "_", value).strip("_")
    if not value:
        value = "unnamed"
    if value[0].isdigit():
        value = "_" + value
    return value.lower()


def pack_bits(bits: list[bool], width: int, height: int) -> bytes:
    row_bytes = (width + 7) // 8
    out = bytearray(row_bytes * height)
    for y in range(height):
        for x in range(width):
            if bits[(y * width) + x]:
                out[(y * row_bytes) + (x // 8)] |= 0x80 >> (x & 7)
    return bytes(out)


def _pbm_tokens(raw: bytes) -> list[bytes]:
    tokens: list[bytes] = []
    i = 0
    n = len(raw)
    while i < n:
        c = raw[i]
        if c in b" \t\r\n":
            i += 1
            continue
        if c == ord("#"):
            while i < n and raw[i] not in b"\r\n":
                i += 1
            continue
        start = i
        while i < n and raw[i] not in b" \t\r\n#":
            i += 1
        tokens.append(raw[start:i])
    return tokens


def load_pbm_bitmap(path: Path, width: int, height: int, invert: bool) -> bytes:
    raw = path.read_bytes()
    if raw.startswith(b"P1"):
        toks = _pbm_tokens(raw)
        if len(toks) < 4:
            raise ValueError(f"invalid P1 PBM: {path}")
        w = int(toks[1])
        h = int(toks[2])
        if (w, h) != (width, height):
            raise ValueError(f"{path} size mismatch: expected {width}x{height}, got {w}x{h}")
        bits = [tok == b"1" for tok in toks[3:]]
        if len(bits) != width * height:
            raise ValueError(f"{path} pixel count mismatch for P1")
        if invert:
            bits = [not bit for bit in bits]
        return pack_bits(bits, width, height)

    if raw.startswith(b"P4"):
        # Parse header manually to keep binary payload intact.
        i = 2
        n = len(raw)
        header_toks: list[bytes] = []
        while i < n and len(header_toks) < 2:
            c = raw[i]
            if c in b" \t\r\n":
                i += 1
                continue
            if c == ord("#"):
                while i < n and raw[i] not in b"\r\n":
                    i += 1
                continue
            start = i
            while i < n and raw[i] not in b" \t\r\n#":
                i += 1
            header_toks.append(raw[start:i])
        if len(header_toks) != 2:
            raise ValueError(f"invalid P4 PBM header: {path}")
        w = int(header_toks[0])
        h = int(header_toks[1])
        if (w, h) != (width, height):
            raise ValueError(f"{path} size mismatch: expected {width}x{height}, got {w}x{h}")
        while i < n and raw[i] in b" \t\r\n":
            i += 1
        payload = raw[i:]
        row_bytes = (width + 7) // 8
        expected = row_bytes * height
        if len(payload) < expected:
            raise ValueError(f"{path} truncated P4 payload")
        payload = payload[:expected]
        if not invert:
            return payload
        inv = bytes((~b) & 0xFF for b in payload)
        return inv

    raise ValueError(f"unsupported PBM format in {path} (expected P1 or P4)")


def load_bitmap(path: Path, width: int, height: int, invert: bool) -> bytes:
    if path.suffix.lower() == ".pbm":
        return load_pbm_bitmap(path, width, height, invert)

    if Image is None:
        raise RuntimeError(
            f"Pillow is required to load image asset {path}. Install with: pip install pillow"
        )

    with Image.open(path) as img:
        img = img.convert("L")
        if img.size != (width, height):
            raise ValueError(
                f"{path} size mismatch: expected {width}x{height}, got {img.size[0]}x{img.size[1]}"
            )
        px = list(img.getdata())

    on_bits = [value >= 128 for value in px]
    if invert:
        on_bits = [not bit for bit in on_bits]
    return pack_bits(on_bits, width, height)


def format_byte_array(data: bytes) -> str:
    cols = 12
    lines: list[str] = []
    for i in range(0, len(data), cols):
        chunk = data[i : i + cols]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    return "\n".join(lines)


@dataclass
class FrameAsset:
    symbol: str
    bitmap: bytes
    duration_ms: int
    source_relpath: str


@dataclass
class AnimationAsset:
    name: str
    symbol: str
    width: int
    height: int
    bit_packed: bool
    frames: list[FrameAsset]


def parse_animation(name: str, cfg: dict[str, Any], assets_root: Path) -> AnimationAsset:
    if not isinstance(cfg, dict):
        raise ValueError(f"animations.{name} must be a mapping")

    width = as_int(cfg.get("width", 128), f"animations.{name}.width")
    height = as_int(cfg.get("height", 64), f"animations.{name}.height")
    bit_packed = as_bool(cfg.get("bit_packed", True), f"animations.{name}.bit_packed")
    invert = as_bool(cfg.get("invert", False), f"animations.{name}.invert")
    frame_interval_ms = as_int(cfg.get("frame_interval_ms", 90), f"animations.{name}.frame_interval_ms")

    if width <= 0 or height <= 0:
        raise ValueError(f"animations.{name} width/height must be > 0")
    if not bit_packed:
        raise ValueError(f"animations.{name} currently supports only bit_packed=true")

    raw_frames = cfg.get("frames", [])
    if not isinstance(raw_frames, list):
        raise ValueError(f"animations.{name}.frames must be a list")

    symbol = sanitize_symbol(name)
    frames: list[FrameAsset] = []
    for idx, entry in enumerate(raw_frames):
        if isinstance(entry, str):
            rel = entry
            duration_ms = frame_interval_ms
        elif isinstance(entry, dict):
            rel = entry.get("path")
            if not isinstance(rel, str) or not rel:
                raise ValueError(f"animations.{name}.frames[{idx}].path must be a non-empty string")
            duration_ms = as_int(entry.get("duration_ms", frame_interval_ms), f"animations.{name}.frames[{idx}].duration_ms")
        else:
            raise ValueError(f"animations.{name}.frames[{idx}] must be string or mapping")

        if duration_ms <= 0:
            raise ValueError(f"animations.{name}.frames[{idx}].duration_ms must be > 0")

        source = (assets_root / rel).resolve()
        if not source.is_file():
            raise FileNotFoundError(f"animation frame not found: {source}")

        frame_symbol = f"g_oled_anim_{symbol}_frame_{idx}"
        bitmap = load_bitmap(source, width, height, invert)
        frames.append(
            FrameAsset(
                symbol=frame_symbol,
                bitmap=bitmap,
                duration_ms=duration_ms,
                source_relpath=rel.replace("\\", "/"),
            )
        )

    return AnimationAsset(
        name=name,
        symbol=symbol,
        width=width,
        height=height,
        bit_packed=bit_packed,
        frames=frames,
    )


def render_header(animations: list[AnimationAsset]) -> str:
    out: list[str] = []
    out.append("// AUTO-GENERATED FILE. DO NOT EDIT.")
    out.append("// Source: assets/animations/manifest.yaml")
    out.append("")
    out.append("#pragma once")
    out.append("")
    out.append('#include "oled.h"')
    out.append("")

    for anim in animations:
        out.append(f"// Animation: {anim.name}")
        for frame in anim.frames:
            out.append(f"//   {frame.source_relpath}")
            out.append(f"static const uint8_t {frame.symbol}[] = {{")
            out.append(format_byte_array(frame.bitmap))
            out.append("};")
            out.append("")

        table_name = f"g_oled_anim_{anim.symbol}_frames"
        if anim.frames:
            out.append(f"static const oled_animation_frame_t {table_name}[] = {{")
            for frame in anim.frames:
                out.append(f"    {{ .bitmap = {frame.symbol}, .duration_ms = {frame.duration_ms} }},")
            out.append("};")
        else:
            out.append(f"static const oled_animation_frame_t *const {table_name} = NULL;")

        anim_var_name = "g_oled_boot_animation" if anim.symbol == "boot" else f"g_oled_animation_{anim.symbol}"
        out.append(f"static const oled_animation_t {anim_var_name} = {{")
        out.append(f"    .width = {anim.width},")
        out.append(f"    .height = {anim.height},")
        out.append(f"    .bit_packed = {'true' if anim.bit_packed else 'false'},")
        out.append(f"    .frame_count = {len(anim.frames)},")
        out.append(f"    .frames = {'NULL' if not anim.frames else table_name},")
        out.append("};")
        out.append("")

    has_boot = any(anim.symbol == "boot" for anim in animations)
    if not has_boot:
        out.append("static const oled_animation_t g_oled_boot_animation = {")
        out.append("    .width = 0, .height = 0, .bit_packed = true, .frame_count = 0, .frames = NULL,")
        out.append("};")
    out.append("")
    return "\n".join(out)


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate OLED animation header")
    parser.add_argument("--manifest", required=True, help="Path to assets/animations/manifest.yaml")
    parser.add_argument("--assets-root", required=True, help="Asset root directory")
    parser.add_argument("--out", required=True, help="Output header path")
    args = parser.parse_args()

    manifest_path = Path(args.manifest).resolve()
    assets_root = Path(args.assets_root).resolve()
    out_path = Path(args.out).resolve()

    if not manifest_path.is_file():
        raise FileNotFoundError(f"manifest not found: {manifest_path}")
    if not assets_root.is_dir():
        raise FileNotFoundError(f"assets root not found: {assets_root}")

    with manifest_path.open("r", encoding="utf-8") as f:
        manifest = yaml.safe_load(f)

    if not isinstance(manifest, dict):
        raise ValueError("manifest root must be a mapping")
    animations_cfg = manifest.get("animations", {})
    if not isinstance(animations_cfg, dict):
        raise ValueError("manifest.animations must be a mapping")

    animations: list[AnimationAsset] = []
    for name, cfg in animations_cfg.items():
        if not isinstance(name, str) or not name:
            raise ValueError("animation names must be non-empty strings")
        animations.append(parse_animation(name, cfg, assets_root))

    rendered = render_header(animations)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(rendered, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
