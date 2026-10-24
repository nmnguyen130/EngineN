# Veil of the Silent — GDD v0.5 Production Architecture & Runtime Asset Pipeline

## 1. Runtime Image & TrueType Font Pipeline (`game/assets/`)
- **Runtime Spritesheet Loading (`ImageLoader.h / .cpp`, `GameAssetPipeline.cpp`)**: Loads `data/sprites/vots_spritesheet.png` (and fallback `.bmp`, `768x1056` RGBA8, `8x11` grid of `96x96` frames) directly from disk into `AssetBank` at boot without any hardcoded C++ array headers.
- **Real TrueType/OpenType Fonts (`data/fonts/`)**:
  - `data/fonts/DejaVuSerifCondensed-Bold.ttf` (BitStream/DejaVu Condensed Bold Serif for HUD & combat text)
  - `data/fonts/NotoSerifDisplay-Bold.ttf` (Google SIL OFL Noto Serif Display Bold for Metroidvania titles & headings)
  - `data/fonts/P052-Bold.otf` (URW Palladio Bold dark-fantasy serif)
  - `data/fonts/LMRomanCaps10-Regular.otf` (Latin Modern Roman Small-Caps)
  - `data/fonts/pixel_game_font.png` & `.bmp` (`512x192` RGBA8, `16x6` grid of `32x32` anti-aliased FreeType cells rasterized from `DejaVuSerifCondensed-Bold.ttf`).
- **Standalone Python Spritesheet & Font CLI (`tools/sprite_tool.py`)**: Supports `split`, `pack`, `slice`, `pad`, `palette`, `preview`, and `font` (`--ttf`).
