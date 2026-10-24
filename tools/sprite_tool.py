#!/usr/bin/env python3
"""
Veil of the Silent (GDD v0.5) — Standalone Aseprite-Style Spritesheet & TrueType Font Atlas CLI Tool
Inspired by Aseprite CLI (--split-tags, --split-layers, --sheet, --sheet-pack, --pad)

Supported Subcommands:
  1. split   : Split a master spritesheet (.png/.bmp) into individual frames
               (frames/<tag>/frame_XX.png), row strips (strips/row_XX_<tag>.png),
               and an Aseprite-compatible JSON metadata file (vots_spritesheet.json).
  2. pack    : Re-pack edited individual frame PNGs or row strips back into the master
               spritesheet (.png + uncompressed 32-bit RGBA .bmp + Aseprite .json) so
               the C++23 runtime loads your edits immediately on next launch without recompiling!
  3. slice   : Extract or replace a single animation row or frame range.
  4. pad     : Resize / re-center sprite cells (e.g. 64x64 -> 96x96 or vice-versa) with
               customizable anchor ('bottom', 'center', 'top').
  5. palette : Inspect unique RGBA colors in a spritesheet or remap one color to another.
  6. preview : Render a 2x/3x labeled contact sheet PNG.
  7. font    : Rasterize a real TrueType/OpenType (.ttf/.otf) font (such as
               data/fonts/DejaVuSerifCondensed-Bold.ttf or NotoSerifDisplay-Bold.ttf)
               into a high-resolution 512x192 (16x6 grid of 32x32 cells) RGBA8 font atlas
               (.png + .bmp) for runtime loading by EngineN.
"""

from __future__ import annotations
import argparse
import json
import struct
import sys
import zlib
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

# Default Veil of the Silent (GDD v0.5) 8x11 (88-frame, 96x96) row tags & frame counts
DEFAULT_VOTS_TAGS = [
    {"row": 0,  "name": "idle_and_parry", "frames": 8, "active_frames": 8, "desc": "Frames 00..05: Idle (6F) | Frames 06..07: Parry Ward (2F)"},
    {"row": 1,  "name": "walk",           "frames": 8, "active_frames": 8, "desc": "Frames 08..15: Walk Cycle (8F, unarmed)"},
    {"row": 2,  "name": "run",            "frames": 8, "active_frames": 8, "desc": "Frames 16..23: Run Cycle (8F, unarmed)"},
    {"row": 3,  "name": "air_and_dash",   "frames": 8, "active_frames": 8, "desc": "Frames 24..25: JumpRise | 26..27: Fall | 28..31: Dash (4F)"},
    {"row": 4,  "name": "atk_combo_1",    "frames": 8, "active_frames": 8, "desc": "Frames 32..39: Hit 1 Diagonal Cross-Slash R->L (8F, 1.90wu reach)"},
    {"row": 5,  "name": "atk_combo_2",    "frames": 8, "active_frames": 8, "desc": "Frames 40..47: Hit 2 Reverse Rising Slash L->R (8F, 2.00wu reach)"},
    {"row": 6,  "name": "atk_combo_3",    "frames": 8, "active_frames": 8, "desc": "Frames 48..55: Hit 3 / Heavy Overhead Cleave (8F, 2.35..2.45wu reach)"},
    {"row": 7,  "name": "enemy_drifter",  "frames": 8, "active_frames": 8, "desc": "Frames 56..63: Dissonant Drifter (Idle 4F + Swipe 4F)"},
    {"row": 8,  "name": "enemy_sentry",   "frames": 8, "active_frames": 8, "desc": "Frames 64..71: Choir Sentry (Float 4F + Fire 4F)"},
    {"row": 9,  "name": "enemy_brute",    "frames": 8, "active_frames": 8, "desc": "Frames 72..79: Resonance Brute (Stomp 4F + Slam 4F)"},
    {"row": 10, "name": "boss_warden",    "frames": 8, "active_frames": 8, "desc": "Frames 80..87: Ash-Sump Warden Boss (Idle 4F + Cleave 4F)"},
]


def save_uncompressed_rgba_png(img: Image.Image, out_path: Path) -> None:
    """
    Save a standard RGBA8 PNG using zlib level=0 (stored/uncompressed DEFLATE blocks)
    and filter_type=0 (None) so both standard PNG viewers (Aseprite, Photoshop, browsers)
    AND the zero-dependency C++23 runtime loader can read it directly without external libs!
    """
    rgba = img.convert("RGBA")
    width, height = rgba.size
    raw_bytes = rgba.tobytes()
    stride = width * 4

    filtered = bytearray((stride + 1) * height)
    pos = 0
    for y in range(height):
        filtered[pos] = 0
        pos += 1
        row_start = y * stride
        filtered[pos:pos + stride] = raw_bytes[row_start:row_start + stride]
        pos += stride

    comp_obj = zlib.compressobj(level=0)
    idat_payload = comp_obj.compress(bytes(filtered)) + comp_obj.flush()

    def make_chunk(chunk_type: bytes, data: bytes) -> bytes:
        crc = zlib.crc32(chunk_type + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + chunk_type + data + struct.pack(">I", crc)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    png_bytes = (
        b"\x89PNG\r\n\x1a\n"
        + make_chunk(b"IHDR", ihdr)
        + make_chunk(b"IDAT", idat_payload)
        + make_chunk(b"IEND", b"")
    )
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(png_bytes)


def save_uncompressed_rgba_bmp(img: Image.Image, out_path: Path) -> None:
    """
    Save an uncompressed 32-bit BGRA top-down BMP (with BITMAPINFOHEADER) for instant
    zero-overhead fread fallback in C++23.
    """
    rgba = img.convert("RGBA")
    width, height = rgba.size
    raw = rgba.tobytes()

    bgra = bytearray(len(raw))
    bgra[0::4] = raw[2::4]  # B
    bgra[1::4] = raw[1::4]  # G
    bgra[2::4] = raw[0::4]  # R
    bgra[3::4] = raw[3::4]  # A

    file_header_size = 14
    info_header_size = 40
    pixel_offset = file_header_size + info_header_size
    file_size = pixel_offset + len(bgra)

    file_hdr = struct.pack("<2sIHHI", b"BM", file_size, 0, 0, pixel_offset)
    info_hdr = struct.pack(
        "<IiiHHIIiiII",
        info_header_size,
        width,
        -height,
        1,
        32,
        0,
        len(bgra),
        2835,
        2835,
        0,
        0,
    )
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(file_hdr + info_hdr + bytes(bgra))


def build_aseprite_json(width: int, height: int, cell_w: int, cell_h: int, cols: int, rows: int) -> dict:
    frames_dict = {}
    frame_tags = []
    for r in range(rows):
        tag_info = DEFAULT_VOTS_TAGS[r] if r < len(DEFAULT_VOTS_TAGS) else {
            "row": r, "name": f"row_{r:02d}", "frames": cols, "desc": ""
        }
        tag_name = tag_info["name"]
        from_idx = r * cols
        to_idx = from_idx + cols - 1
        frame_tags.append({
            "name": tag_name,
            "from": from_idx,
            "to": to_idx,
            "direction": "forward",
            "description": tag_info.get("desc", ""),
        })
        for c in range(cols):
            idx = r * cols + c
            fname = f"{tag_name}_{c:02d}.png"
            frames_dict[fname] = {
                "frame": {"x": c * cell_w, "y": r * cell_h, "w": cell_w, "h": cell_h},
                "rotated": False,
                "trimmed": False,
                "spriteSourceSize": {"x": 0, "y": 0, "w": cell_w, "h": cell_h},
                "sourceSize": {"w": cell_w, "h": cell_h},
                "duration": 83,
                "frameIndex": idx,
                "row": r,
                "col": c,
            }
    return {
        "frames": frames_dict,
        "meta": {
            "app": "VeilOfTheSilent-SpriteTool",
            "version": "0.5.0",
            "image": "vots_spritesheet.png",
            "format": "RGBA8888",
            "size": {"w": width, "h": height},
            "cellSize": {"w": cell_w, "h": cell_h},
            "grid": {"cols": cols, "rows": rows},
            "scale": "1",
            "frameTags": frame_tags,
        },
    }


def cmd_split(args: argparse.Namespace) -> None:
    sheet_path = Path(args.input)
    out_dir = Path(args.output)
    cell_w, cell_h = args.cell_w, args.cell_h

    img = Image.open(sheet_path).convert("RGBA")
    width, height = img.size
    cols = width // cell_w
    rows = height // cell_h

    frames_dir = out_dir / "frames"
    strips_dir = out_dir / "strips"
    frames_dir.mkdir(parents=True, exist_ok=True)
    strips_dir.mkdir(parents=True, exist_ok=True)

    for r in range(rows):
        tag_name = DEFAULT_VOTS_TAGS[r]["name"] if r < len(DEFAULT_VOTS_TAGS) else f"row_{r:02d}"
        row_strip = img.crop((0, r * cell_h, width, (r + 1) * cell_h))
        strip_file = strips_dir / f"row_{r:02d}_{tag_name}.png"
        row_strip.save(strip_file)

        tag_folder = frames_dir / f"{r:02d}_{tag_name}"
        tag_folder.mkdir(parents=True, exist_ok=True)
        for c in range(cols):
            cell = img.crop((c * cell_w, r * cell_h, (c + 1) * cell_w, (r + 1) * cell_h))
            cell.save(tag_folder / f"frame_{c:02d}.png")

    meta = build_aseprite_json(width, height, cell_w, cell_h, cols, rows)
    json_path = out_dir / "vots_spritesheet.json"
    json_path.write_text(json.dumps(meta, indent=2), encoding="utf-8")

    save_uncompressed_rgba_png(img, out_dir / "vots_spritesheet.png")
    save_uncompressed_rgba_bmp(img, out_dir / "vots_spritesheet.bmp")
    print(f"[sprite_tool:split] Split {sheet_path} ({width}x{height}, {cols}x{rows} of {cell_w}x{cell_h}) -> {out_dir}")


def cmd_pack(args: argparse.Namespace) -> None:
    in_dir = Path(args.input)
    out_png = Path(args.output)
    cell_w, cell_h = args.cell_w, args.cell_h
    cols, rows = args.cols, args.rows

    sheet = Image.new("RGBA", (cols * cell_w, rows * cell_h), (0, 0, 0, 0))
    frames_dir = in_dir / "frames"
    strips_dir = in_dir / "strips"

    for r in range(rows):
        tag_name = DEFAULT_VOTS_TAGS[r]["name"] if r < len(DEFAULT_VOTS_TAGS) else f"row_{r:02d}"
        tag_folder = frames_dir / f"{r:02d}_{tag_name}"
        strip_file = strips_dir / f"row_{r:02d}_{tag_name}.png"

        if tag_folder.exists():
            for c in range(cols):
                f_path = tag_folder / f"frame_{c:02d}.png"
                if f_path.exists():
                    cell = Image.open(f_path).convert("RGBA")
                    if cell.size != (cell_w, cell_h):
                        cell = cell.resize((cell_w, cell_h), Image.Resampling.NEAREST)
                    sheet.paste(cell, (c * cell_w, r * cell_h))
        elif strip_file.exists():
            strip = Image.open(strip_file).convert("RGBA")
            sheet.paste(strip, (0, r * cell_h))

    save_uncompressed_rgba_png(sheet, out_png)
    out_bmp = out_png.with_suffix(".bmp")
    save_uncompressed_rgba_bmp(sheet, out_bmp)
    meta = build_aseprite_json(cols * cell_w, rows * cell_h, cell_w, cell_h, cols, rows)
    out_png.with_suffix(".json").write_text(json.dumps(meta, indent=2), encoding="utf-8")
    print(f"[sprite_tool:pack] Packed {cols}x{rows} ({cell_w}x{cell_h}) -> {out_png} & {out_bmp}")


def cmd_slice(args: argparse.Namespace) -> None:
    sheet = Image.open(args.input).convert("RGBA")
    x, y, w, h = args.x, args.y, args.w, args.h
    sub = sheet.crop((x, y, x + w, y + h))
    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    sub.save(out_path)
    print(f"[sprite_tool:slice] Extracted rect ({x},{y},{w},{h}) -> {out_path}")


def cmd_pad(args: argparse.Namespace) -> None:
    sheet = Image.open(args.input).convert("RGBA")
    old_w, old_h = args.old_w, args.old_h
    new_w, new_h = args.new_w, args.new_h
    cols = sheet.width // old_w
    rows = sheet.height // old_h

    out_sheet = Image.new("RGBA", (cols * new_w, rows * new_h), (0, 0, 0, 0))
    for r in range(rows):
        for c in range(cols):
            cell = sheet.crop((c * old_w, r * old_h, (c + 1) * old_w, (r + 1) * old_h))
            dx = (new_w - old_w) // 2
            if args.anchor == "bottom":
                dy = new_h - old_h
            elif args.anchor == "top":
                dy = 0
            else:
                dy = (new_h - old_h) // 2
            out_sheet.paste(cell, (c * new_w + dx, r * new_h + dy))

    out_path = Path(args.output)
    save_uncompressed_rgba_png(out_sheet, out_path)
    save_uncompressed_rgba_bmp(out_sheet, out_path.with_suffix(".bmp"))
    print(f"[sprite_tool:pad] Padded {old_w}x{old_h} -> {new_w}x{new_h} ({args.anchor}) at {out_path}")


def cmd_palette(args: argparse.Namespace) -> None:
    img = Image.open(args.input).convert("RGBA")
    pixels = list(img.getdata())
    counts: dict[tuple[int, int, int, int], int] = {}
    for px in pixels:
        if px[3] > 0:
            counts[px] = counts.get(px, 0) + 1
    sorted_cols = sorted(counts.items(), key=lambda kv: kv[1], reverse=True)
    print(f"[sprite_tool:palette] {args.input}: {len(sorted_cols)} unique non-transparent RGBA colors")
    for (r, g, b, a), cnt in sorted_cols[:24]:
        print(f"  #{r:02X}{g:02X}{b:02X}{a:02X} : {cnt:6d} px")

    if args.replace_from and args.replace_to and args.output:
        rf = tuple(int(args.replace_from.lstrip("#")[i:i + 2], 16) for i in (0, 2, 4))
        rt = tuple(int(args.replace_to.lstrip("#")[i:i + 2], 16) for i in (0, 2, 4))
        new_px = [
            (rt[0], rt[1], rt[2], px[3]) if (px[3] > 0 and (px[0], px[1], px[2]) == rf) else px
            for px in pixels
        ]
        out_img = Image.new("RGBA", img.size)
        out_img.putdata(new_px)
        save_uncompressed_rgba_png(out_img, Path(args.output))
        print(f"[sprite_tool:palette] Replaced #{rf[0]:02X}{rf[1]:02X}{rf[2]:02X} -> #{rt[0]:02X}{rt[1]:02X}{rt[2]:02X} in {args.output}")


def cmd_preview(args: argparse.Namespace) -> None:
    img = Image.open(args.input).convert("RGBA")
    scale = args.scale
    cell_w, cell_h = args.cell_w, args.cell_h
    cols = img.width // cell_w
    rows = img.height // cell_h

    scaled = img.resize((img.width * scale, img.height * scale), Image.Resampling.NEAREST)
    canvas = Image.new("RGBA", (scaled.width, scaled.height), (18, 22, 32, 255))
    draw = ImageDraw.Draw(canvas)

    step = 16 * scale
    for y in range(0, canvas.height, step):
        for x in range(0, canvas.width, step):
            if ((x // step) + (y // step)) % 2 == 1:
                draw.rectangle([x, y, x + step - 1, y + step - 1], fill=(24, 29, 42, 255))
    canvas.alpha_composite(scaled)
    draw = ImageDraw.Draw(canvas)
    for r in range(rows + 1):
        y = r * cell_h * scale
        draw.line([(0, y), (canvas.width, y)], fill=(58, 72, 98, 180), width=1)
    for c in range(cols + 1):
        x = c * cell_w * scale
        draw.line([(x, 0), (x, canvas.height)], fill=(58, 72, 98, 180), width=1)

    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(out_path)
    print(f"[sprite_tool:preview] Saved {scale}x contact preview -> {out_path}")


def cmd_font(args: argparse.Namespace) -> None:
    """
    Rasterize a real TrueType/OpenType (.ttf / .otf) font file (such as
    data/fonts/DejaVuSerifCondensed-Bold.ttf or data/fonts/NotoSerifDisplay-Bold.ttf)
    into a high-resolution 512x192 RGBA8 font atlas (16 cols x 6 rows of 32x32 cells,
    ASCII 32..127) with smooth FreeType anti-aliasing, dark outline/drop-shadow, and
    subtle top-to-bottom silver/ivory gradient so EngineN renders genuine serif typography!
    """
    out_dir = Path(args.output)
    out_dir.mkdir(parents=True, exist_ok=True)

    ttf_candidates = [
        Path(args.ttf) if args.ttf else None,
        out_dir / "DejaVuSerifCondensed-Bold.ttf",
        out_dir / "NotoSerifDisplay-Bold.ttf",
        out_dir / "P052-Bold.otf",
        Path("/usr/share/fonts/truetype/dejavu/DejaVuSerifCondensed-Bold.ttf"),
        Path("/usr/share/fonts/truetype/noto/NotoSerifDisplay-Bold.ttf"),
    ]
    chosen_ttf: Path | None = None
    for cand in ttf_candidates:
        if cand is not None and cand.exists():
            chosen_ttf = cand
            break
    if chosen_ttf is None:
        raise FileNotFoundError("Could not locate a real .ttf/.otf font file in data/fonts/")

    cell_w, cell_h = 32, 32
    cols, rows = 16, 6
    atlas_w, atlas_h = cols * cell_w, rows * cell_h  # 512x192

    font = ImageFont.truetype(str(chosen_ttf), size=23)
    img = Image.new("RGBA", (atlas_w, atlas_h), (0, 0, 0, 0))

    for ch in range(32, 128):
        idx = ch - 32
        col = idx % cols
        row = idx // cols
        char_str = chr(ch) if ch < 127 else ""
        if not char_str or ch == 32:
            continue

        cell_img = Image.new("RGBA", (cell_w, cell_h), (0, 0, 0, 0))
        cdraw = ImageDraw.Draw(cell_img)

        # Measure glyph bounding box and center horizontally, align baseline at y=25
        bbox = cdraw.textbbox((0, 0), char_str, font=font)
        gw = bbox[2] - bbox[0]
        tx = max(2, (cell_w - gw) // 2 - bbox[0])
        ty = 3

        # 1. Crisp dark-fantasy outline + drop shadow for instant readability over any background
        for ox, oy in [(-1, 0), (1, 0), (0, -1), (0, 1), (1, 2), (2, 2)]:
            cdraw.text((tx + ox, ty + oy), char_str, font=font, fill=(14, 16, 26, 235))

        # 2. Render anti-aliased glyph mask from real TrueType font
        mask_img = Image.new("L", (cell_w, cell_h), 0)
        mdraw = ImageDraw.Draw(mask_img)
        mdraw.text((tx, ty), char_str, font=font, fill=255)

        # Apply subtle ivory-silver vertical sheen to the TrueType glyph body
        cpx = cell_img.load()
        mpx = mask_img.load()
        for y in range(cell_h):
            t = y / float(cell_h - 1)
            r_col = int(255 * (1.0 - 0.06 * t))
            g_col = int(255 * (1.0 - 0.08 * t))
            b_col = 255
            for x in range(cell_w):
                a = mpx[x, y]
                if a > 0:
                    bg_r, bg_g, bg_b, bg_a = cpx[x, y]
                    alpha_f = a / 255.0
                    out_r = int(r_col * alpha_f + bg_r * (1.0 - alpha_f))
                    out_g = int(g_col * alpha_f + bg_g * (1.0 - alpha_f))
                    out_b = int(b_col * alpha_f + bg_b * (1.0 - alpha_f))
                    out_a = max(bg_a, a)
                    cpx[x, y] = (out_r, out_g, out_b, out_a)

        img.paste(cell_img, (col * cell_w, row * cell_h))

    png_path = out_dir / "pixel_game_font.png"
    bmp_path = out_dir / "pixel_game_font.bmp"
    json_path = out_dir / "pixel_game_font.json"
    save_uncompressed_rgba_png(img, png_path)
    save_uncompressed_rgba_bmp(img, bmp_path)
    json_path.write_text(
        json.dumps(
            {
                "sourceTrueTypeFont": chosen_ttf.name,
                "atlasWidth": atlas_w,
                "atlasHeight": atlas_h,
                "cellWidth": cell_w,
                "cellHeight": cell_h,
                "columns": cols,
                "rows": rows,
                "firstAscii": 32,
                "lastAscii": 127,
            },
            indent=2,
        ),
        encoding="utf-8",
    )
    print(f"[sprite_tool:font] Rasterized real TrueType font '{chosen_ttf.name}' -> {png_path} & {bmp_path} ({atlas_w}x{atlas_h})")


def main() -> int:
    parser = argparse.ArgumentParser(description="Veil of the Silent — Aseprite-style Spritesheet & TrueType Font Tool")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_split = sub.add_parser("split", help="Split spritesheet into frames, row strips, and Aseprite JSON")
    p_split.add_argument("-i", "--input", required=True, help="Input spritesheet PNG/BMP")
    p_split.add_argument("-o", "--output", required=True, help="Output directory")
    p_split.add_argument("--cell-w", type=int, default=96)
    p_split.add_argument("--cell-h", type=int, default=96)
    p_split.set_defaults(func=cmd_split)

    p_pack = sub.add_parser("pack", help="Pack edited frames/strips into master runtime spritesheet (.png & .bmp)")
    p_pack.add_argument("-i", "--input", required=True, help="Directory containing frames/ or strips/")
    p_pack.add_argument("-o", "--output", required=True, help="Output master PNG path")
    p_pack.add_argument("--cell-w", type=int, default=96)
    p_pack.add_argument("--cell-h", type=int, default=96)
    p_pack.add_argument("--cols", type=int, default=8)
    p_pack.add_argument("--rows", type=int, default=11)
    p_pack.set_defaults(func=cmd_pack)

    p_slice = sub.add_parser("slice", help="Crop a rectangular region from a spritesheet")
    p_slice.add_argument("-i", "--input", required=True)
    p_slice.add_argument("-o", "--output", required=True)
    p_slice.add_argument("--x", type=int, required=True)
    p_slice.add_argument("--y", type=int, required=True)
    p_slice.add_argument("--w", type=int, required=True)
    p_slice.add_argument("--h", type=int, required=True)
    p_slice.set_defaults(func=cmd_slice)

    p_pad = sub.add_parser("pad", help="Resize/re-center sprite grid cells (e.g. 64x64 <-> 96x96)")
    p_pad.add_argument("-i", "--input", required=True)
    p_pad.add_argument("-o", "--output", required=True)
    p_pad.add_argument("--old-w", type=int, default=64)
    p_pad.add_argument("--old-h", type=int, default=64)
    p_pad.add_argument("--new-w", type=int, default=96)
    p_pad.add_argument("--new-h", type=int, default=96)
    p_pad.add_argument("--anchor", choices=["bottom", "center", "top"], default="bottom")
    p_pad.set_defaults(func=cmd_pad)

    p_pal = sub.add_parser("palette", help="List or replace RGBA palette colors")
    p_pal.add_argument("-i", "--input", required=True)
    p_pal.add_argument("-o", "--output", default="")
    p_pal.add_argument("--replace-from", default="")
    p_pal.add_argument("--replace-to", default="")
    p_pal.set_defaults(func=cmd_palette)

    p_prev = sub.add_parser("preview", help="Generate scaled contact sheet with grid lines")
    p_prev.add_argument("-i", "--input", required=True)
    p_prev.add_argument("-o", "--output", required=True)
    p_prev.add_argument("--cell-w", type=int, default=96)
    p_prev.add_argument("--cell-h", type=int, default=96)
    p_prev.add_argument("--scale", type=int, default=2)
    p_prev.set_defaults(func=cmd_preview)

    p_font = sub.add_parser("font", help="Rasterize a real .ttf/.otf font into a 512x192 game font atlas (.png & .bmp)")
    p_font.add_argument("-o", "--output", required=True, help="Output font directory (e.g. data/fonts)")
    p_font.add_argument("--ttf", default="", help="Optional path to .ttf/.otf file")
    p_font.set_defaults(func=cmd_font)

    args = parser.parse_args()
    args.func(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())