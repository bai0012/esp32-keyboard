# OLED Animation Assets

This folder stores OLED animations for startup and future menu/UI transitions.

## Structure
- `manifest.yaml`: animation index and timing metadata.
- `boot/`: boot animation source frames.
- future folders can be added for menu/page animations.

## Supported frame formats
- `.pbm` (P1/P4, native parser, no extra dependency)
- `.png`, `.bmp`, `.jpg`, `.jpeg` (via Pillow in build Python env)

All frames are converted at build time to 1-bit packed bitmaps.

## Required frame size
- Current OLED panel: `128x64`.
- Keep all frames in one animation at the same resolution.

## Build integration
`tools/generate_oled_animation_header.py` generates:
- `main/oled_animation_assets.h`

from `manifest.yaml` + frame files.
