#!/usr/bin/env python3
"""Compile original drawable leaves to indexed or lossless RGBA row packs.

Build-time only: Python/Pillow + Java/JPEXS. No runtime Flash, Java or images.
The temporary SWF displays exactly one leaf per frame and contains no scripts.
Coordinates are preserved by translating bounds onto an integer raster grid.
"""

from __future__ import annotations
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
import xml.etree.ElementTree as ET
import zlib
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
DECOMP = ROOT.parent / "pico-decomp"
REC = struct.Struct("<HHhhHHIII")


class Bits:
    def __init__(self):
        self.bits = []

    def put(self, n, v):
        self.bits.extend((v >> i) & 1 for i in range(n - 1, -1, -1))

    def data(self):
        self.bits += [0] * (-len(self.bits) % 8)
        return bytes(
            sum(self.bits[i + j] << (7 - j) for j in range(8)) for i in range(0, len(self.bits), 8)
        )


def signed_bits(v):
    return max(1, max((x.bit_length() + 1 if x >= 0 else (~x).bit_length() + 1) for x in v))


def rect(x0, x1, y0, y1):
    b = Bits()
    n = signed_bits((x0, x1, y0, y1))
    b.put(5, n)
    for v in (x0, x1, y0, y1):
        b.put(n, v)
    return b.data()


def matrix(tx, ty):
    b = Bits()
    b.put(1, 0)
    b.put(1, 0)
    n = signed_bits((tx, ty))
    b.put(5, n)
    b.put(n, tx)
    b.put(n, ty)
    return b.data()


def tag(code, body=b""):
    n = len(body)
    return (
        struct.pack("<H", code * 64 + min(n, 63))
        + (struct.pack("<I", n) if n >= 63 else b"")
        + body
    )


def source_tags(path):
    data = path.read_bytes()
    if data[:3] == b"CWS":
        data = b"FWS" + data[3:8] + zlib.decompress(data[8:])
    if data[:3] != b"FWS":
        raise ValueError("Only FWS/CWS accepted")
    p = 8 + math.ceil((5 + 4 * (data[8] >> 3)) / 8) + 4
    while p < len(data):
        start = p
        h = struct.unpack_from("<H", data, p)[0]
        p += 2
        n = h & 63
        if n == 63:
            n = struct.unpack_from("<I", data, p)[0]
            p += 4
        yield h >> 6, data[p : p + n], data[start : p + n]
        p += n


def authored_morph_ratios(path):
    structure = json.loads(path.read_text(encoding="utf-8"))
    ratios = {
        int(character["id"]): {0, 65535}
        for character in structure["characters"]
        if "MorphShape" in character["kind"]
    }
    for timeline in structure["timelines"]:
        for frame in timeline["frames"]:
            for placement in frame["display_list"]:
                symbol = placement["character_id"]
                if symbol in ratios:
                    ratio = int(placement.get("ratio", 0))
                    if not 0 <= ratio <= 65535:
                        raise ValueError(f"Invalid morph ratio {symbol}: {ratio}")
                    ratios[symbol].add(ratio)
    return {symbol: sorted(values) for symbol, values in sorted(ratios.items())}


def leaf_metadata(xml, scale_num, scale_den, morph_steps, morph_ratios=None):
    leaves = []
    scale = scale_num / scale_den
    for e in ET.parse(xml).getroot().iter("item"):
        kind = e.get("type", "")
        if not any(
            kind.startswith(t)
            for t in ("DefineShape", "DefineText", "DefineEditText", "DefineMorphShape")
        ):
            continue
        id_ = int(e.get("shapeId", e.get("characterID", e.get("characterId", "0"))))
        bounds = [
            c
            for c in e
            if c.tag in ("shapeBounds", "textBounds", "bounds", "startBounds", "endBounds")
        ]
        if not bounds:
            raise ValueError(f"Missing bounds {id_}")
        x0 = math.floor(min(int(c.get("Xmin")) for c in bounds) / 20 * scale) - 2
        x1 = math.ceil(max(int(c.get("Xmax")) for c in bounds) / 20 * scale) + 2
        y0 = math.floor(min(int(c.get("Ymin")) for c in bounds) / 20 * scale) - 2
        y1 = math.ceil(max(int(c.get("Ymax")) for c in bounds) / 20 * scale) + 2
        if "Morph" in kind:
            ratios = (
                morph_ratios[id_] if morph_ratios is not None
                else [round(65535 * i / (morph_steps - 1)) for i in range(morph_steps)]
            )
        else:
            ratios = [0]
        for ratio in ratios:
            leaves.append(
                dict(
                    symbol=id_,
                    ratio=ratio,
                    kind=kind,
                    origin=[x0, y0],
                    size=[max(1, x1 - x0), max(1, y1 - y0)],
                )
            )
    return sorted(leaves, key=lambda e: (e["symbol"], e["ratio"]))


def make_wrapper(source, leaves, path, scale_num, scale_den):
    # Include only graphic/font definitions; deliberately omit timelines/buttons
    # and every action, sound, network and initialization tag.
    keep = {
        2,
        6,
        8,
        10,
        11,
        13,
        20,
        21,
        22,
        32,
        33,
        35,
        36,
        37,
        46,
        48,
        62,
        73,
        74,
        75,
        83,
        84,
        88,
        90,
        91,
    }
    tags = b"".join(raw for code, body, raw in source_tags(source) if code in keep)
    width = max(e["size"][0] for e in leaves)
    height = max(e["size"][1] for e in leaves)
    for index, e in enumerate(leaves):
        tx = round(-e["origin"][0] * 20 * scale_den / scale_num)
        ty = round(-e["origin"][1] * 20 * scale_den / scale_num)
        place = (
            bytes([0x16])
            + struct.pack("<HH", 1, e["symbol"])
            + matrix(tx, ty)
            + struct.pack("<H", e["ratio"])
        )
        tags += tag(26, place) + tag(1) + tag(28, struct.pack("<H", 1))
        e["frame"] = index + 1
    tags += tag(0)
    body = (
        rect(
            0,
            math.ceil(width * 20 * scale_den / scale_num),
            0,
            math.ceil(height * 20 * scale_den / scale_num),
        )
        + struct.pack("<HH", 24 * 256, len(leaves))
        + tags
    )
    path.write_bytes(b"FWS" + bytes([8]) + struct.pack("<I", len(body) + 8) + body)


def rle_row(data):
    out = bytearray()
    i = 0
    while i < len(data):
        n = 1
        while n < 255 and i + n < len(data) and data[i + n] == data[i]:
            n += 1
        out.extend((n, data[i]))
        i += n
    return bytes(out)


def rle_rgba_row(data):
    if len(data) % 4:
        raise ValueError("RGBA rows must contain complete pixels")
    out = bytearray()
    i = 0
    while i < len(data):
        pixel = data[i : i + 4]
        n = 1
        while n < 255 and i + n * 4 < len(data) and data[i + n * 4 : i + (n + 1) * 4] == pixel:
            n += 1
        out.append(n)
        out.extend(pixel)
        i += n * 4
    return bytes(out)


def compress_rgba_row(data, lz4_block):
    if not data or len(data) % 4:
        raise ValueError("RGBA rows must contain complete pixels")
    planes = [data[channel::4] for channel in range(4)]
    deltas = [
        plane[:1] + bytes((b - a) & 255 for a, b in zip(plane, plane[1:]))
        for plane in planes
    ]
    interleaved_delta = bytearray(len(data))
    for channel, delta in enumerate(deltas):
        interleaved_delta[channel::4] = delta
    candidates = [b"\x00" + data, b"\x01" + rle_rgba_row(data)]
    for mode, filtered in enumerate(
        (data, b"".join(planes), interleaved_delta, b"".join(deltas)), start=2
    ):
        block = lz4_block.compress(
            filtered, mode="high_compression", compression=12, store_size=False
        )
        candidates.append(bytes([mode]) + block)
    return min(candidates, key=len)


def compile_pack(leaves, frames, output, scale_num, scale_den, images_dir, format_="indexed"):
    rgba = format_ == "rgba"
    if rgba:
        import lz4
        import lz4.block
    artwork = []
    for e in leaves:
        path = frames / f"{e['frame']}.png"
        if not path.exists():
            matches = list(frames.glob(f"*{e['frame']:04d}*.png"))
            if len(matches) != 1:
                raise FileNotFoundError(path)
            path = matches[0]
        im = Image.open(path).convert("RGBA").crop((0, 0, *e["size"]))
        alpha = im.getchannel("A")
        bbox = alpha.getbbox() if rgba else alpha.point(lambda x: 255 if x >= 128 else 0).getbbox()
        if bbox:
            im = im.crop(bbox)
            e["origin"] = [e["origin"][0] + bbox[0], e["origin"][1] + bbox[1]]
        else:
            im = Image.new("RGBA", (1, 1))
            e["empty"] = True
        e["size"] = list(im.size)
        artwork.append(im)
    # Bitmap definitions are normally used as shape fills, but retain them for
    # completeness and direct symbol hit lookups without another image decoder.
    for path in sorted(images_dir.glob("*.png")):
        id_ = int(path.stem.split("_")[0])
        im = Image.open(path).convert("RGBA")
        size = (
            max(1, round(im.width * scale_num / scale_den)),
            max(1, round(im.height * scale_num / scale_den)),
        )
        im = im.resize(size, Image.Resampling.LANCZOS)
        leaves.append(dict(symbol=id_, ratio=0, kind="bitmap", origin=[0, 0], size=list(size)))
        artwork.append(im)
    ordered = sorted(zip(leaves, artwork), key=lambda p: (p[0]["symbol"], p[0]["ratio"]))
    leaves = [e for e, im in ordered]
    artwork = [im for e, im in ordered]
    if not rgba:
        # Visible-pixel histogram, bounded deterministic sample weighted by frequency.
        colors = Counter()
        for im in artwork:
            get_pixels = getattr(im, "get_flattened_data", im.getdata)
            colors.update((r, g, b) for r, g, b, a in get_pixels() if a >= 128)
        pixels = []
        divisor = max(1, sum(colors.values()) // 1000000)
        for color, count in sorted(colors.items()):
            pixels.extend([color] * max(1, count // divisor))
        sample = Image.new("RGB", (len(pixels), 1))
        sample.putdata(pixels)
        palim = sample.quantize(colors=255, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
        palette = palim.getpalette()[: 255 * 3]
        palref = Image.new("P", (1, 1))
        palref.putpalette(palette + palette[:3])
        packed_palette = bytearray(512)
        for i in range(255):
            r, g, b = palette[i * 3 : i * 3 + 3]
            struct.pack_into(
                "<H", packed_palette, (i + 1) * 2, ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            )
    record_offset = 32
    records_end = record_offset + len(leaves) * REC.size
    palette_offset = 0 if rgba else records_end
    blob = bytearray(records_end if rgba else records_end + 512)
    blob[:4] = b"PCTA"
    struct.pack_into(
        "<HHHHII", blob, 4, 3 if rgba else 1, len(leaves), scale_num, scale_den, record_offset, palette_offset
    )
    if not rgba:
        blob[palette_offset : palette_offset + 512] = packed_palette
    dedup = {}
    total_pixels = 0
    unique = 0
    row_modes = [0] * 6
    for i, (e, im) in enumerate(zip(leaves, artwork)):
        if rgba:
            encoded_pixels = im.tobytes()
            e["rgba_sha256"] = hashlib.sha256(encoded_pixels).hexdigest()
        else:
            q = im.convert("RGB").quantize(palette=palref, dither=Image.Dither.NONE).tobytes()
            alpha = im.getchannel("A").tobytes()
            encoded_pixels = bytes((c % 255 + 1 if a >= 128 else 0) for c, a in zip(q, alpha))
        w, h = im.size
        total_pixels += w * h
        key = (w, h, encoded_pixels)
        if key in dedup:
            rowstart, pixstart, end = dedup[key]
        else:
            unique += 1
            rowstart = len(blob)
            blob += bytes((h + 1) * 4)
            pixstart = len(blob)
            for y in range(h):
                struct.pack_into("<I", blob, rowstart + y * 4, len(blob))
                if rgba:
                    row = compress_rgba_row(encoded_pixels[y * w * 4 : (y + 1) * w * 4], lz4.block)
                    row_modes[row[0]] += 1
                    blob += row
                else:
                    blob += rle_row(encoded_pixels[y * w : (y + 1) * w])
            end = len(blob)
            struct.pack_into("<I", blob, rowstart + h * 4, end)
            dedup[key] = (rowstart, pixstart, end)
        REC.pack_into(
            blob,
            record_offset + i * REC.size,
            e["symbol"],
            e["ratio"],
            *e["origin"],
            w,
            h,
            rowstart,
            pixstart,
            end,
        )
        e.update({"encoded_bytes" if rgba else "rle_bytes": end - pixstart,
                  "row_table_bytes": 4 * (h + 1), "index": i})
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(blob)
    report = {
        "format": "PCTA3" if rgba else "PCTA1",
        "source_stage": [550, 350],
        "raster_scale": [scale_num, scale_den],
        "asset_records": len(leaves),
        "unique_rasters": unique,
        "symbols": len({e["symbol"] for e in leaves}),
        "pack_bytes": len(blob),
        "pack_sha256": hashlib.sha256(blob).hexdigest(),
        "uncompressed_index8_pixels": total_pixels,
        "palette_colors": 255,
        "alpha": "binary coverage cutoff128; instance alpha/color transform retained at runtime",
        "morph": "nearest among endpoint-inclusive quantized variants",
        "asset_list": leaves,
    }
    if rgba:
        del report["uncompressed_index8_pixels"]
        report.update(
            pixel_format="RGBA8888",
            row_encoding="mode:u8 then independent row payload; raw RGBA, RGBA RLE or LZ4 block",
            row_modes={str(mode): count for mode, count in enumerate(row_modes)},
            lz4_encoder=f"python-lz4 {lz4.__version__}; high_compression level12; store_size=false",
            max_source_row_pixels=max(asset["size"][0] for asset in leaves),
            uncompressed_rgba_pixels=total_pixels,
            uncompressed_rgba_bytes=total_pixels * 4,
            palette_colors=0,
            alpha="original 8-bit straight alpha; no coverage cutoff",
            colour="original rendered RGBA bytes; lossless row compression without a palette",
            rasterisation="source vectors rasterised by JPEXS at capture scale; not a vector runtime",
        )
    output.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=DECOMP / "original/flash_picosschool.swf")
    p.add_argument("--xml", type=Path, default=DECOMP / "exports/movie.xml")
    p.add_argument("--structure", type=Path, default=DECOMP / "analysis/structure.json")
    p.add_argument("--images", type=Path, default=DECOMP / "exports/images")
    p.add_argument("--ffdec", type=Path, default=DECOMP / "vendor/ffdec/ffdec.jar")
    p.add_argument(
        "--java",
        default=shutil.which("java") or r"C:\Program Files\Android\Android Studio\jbr\bin\java.exe",
    )
    p.add_argument("--output", type=Path, default=ROOT / "assets/pico_art.pcta")
    p.add_argument("--work", type=Path, default=ROOT / "build/assets")
    p.add_argument("--scale-num", type=int, default=2)
    p.add_argument("--scale-den", type=int, default=5)
    p.add_argument("--morph-steps", type=int, default=9)
    p.add_argument("--format", choices=("indexed", "rgba"), default="indexed")
    p.add_argument("--sampled-morphs", action="store_true", help="Use --morph-steps instead of authored poses")
    p.add_argument("--reuse-renders", action="store_true")
    args = p.parse_args()
    if args.scale_num < 1 or args.scale_den < 1 or args.morph_steps < 2:
        p.error("Invalid scale or morph steps")
    if args.format == "rgba":
        try:
            import lz4.block
        except ModuleNotFoundError:
            p.error("RGBA packing requires python-lz4. Install lz4==4.4.5 before rebuilding.")
    args.work.mkdir(parents=True, exist_ok=True)
    cache_key = {
        "source_sha256": hashlib.sha256(args.source.read_bytes()).hexdigest(),
        "xml_sha256": hashlib.sha256(args.xml.read_bytes()).hexdigest(),
        "ffdec_sha256": hashlib.sha256(args.ffdec.read_bytes()).hexdigest(),
        "scale": [args.scale_num, args.scale_den],
        "morph_steps": args.morph_steps,
    }
    morph_ratios = None
    if args.format == "rgba" and not args.sampled_morphs:
        if not args.structure.exists():
            p.error("Authored morph poses require --structure. Use --sampled-morphs to sample poses instead.")
        morph_ratios = authored_morph_ratios(args.structure)
        cache_key["structure_sha256"] = hashlib.sha256(args.structure.read_bytes()).hexdigest()
        cache_key["morph_ratios"] = {str(symbol): values for symbol, values in morph_ratios.items()}
    cache = args.work / "render-cache.json"
    if args.reuse_renders and (not cache.exists() or json.loads(cache.read_text()) != cache_key):
        p.error(
            "Cached renders do not match source/XML/rasterizer/profile. Run without --reuse-renders."
        )
    leaves = leaf_metadata(args.xml, args.scale_num, args.scale_den, args.morph_steps, morph_ratios)
    wrapper = args.work / "leaves.swf"
    make_wrapper(args.source, leaves, wrapper, args.scale_num, args.scale_den)
    (args.work / "leaves.json").write_text(json.dumps(leaves, indent=2), encoding="utf-8")
    if not args.reuse_renders:
        print(f"Rendering {len(leaves)} isolated drawable leaves", flush=True)
        command = [
            args.java,
            "-Djava.awt.headless=true",
            "-Xmx2g",
            "-jar",
            str(args.ffdec),
            "-onerror",
            "abort",
            "-ignorebackground",
            "-zoom",
            str(args.scale_num / args.scale_den),
            "-format",
            "frame:png",
            "-export",
            "frame",
            str(args.work / "rendered"),
            str(wrapper),
        ]
        with (args.work / "render.log").open("wb") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        cache.write_text(json.dumps(cache_key, indent=2) + "\n", encoding="utf-8")
    frames = args.work / "rendered/frames"
    if not frames.exists():
        frames = args.work / "rendered"
    result = compile_pack(leaves, frames, args.output, args.scale_num, args.scale_den, args.images, args.format)
    result["source_sha256"] = hashlib.sha256(args.source.read_bytes()).hexdigest()
    result["rasterizer"] = "JPEXS26.2.1 scripts-free isolated-leaf SWF rendering"
    if args.format == "rgba":
        result["capture_scale"] = [args.scale_num, args.scale_den]
        result["capture_stage"] = [550 * args.scale_num / args.scale_den, 350 * args.scale_num / args.scale_den]
        if morph_ratios is None:
            result["morph_mode"] = "sampled"
            result["morph_samples"] = args.morph_steps
        else:
            result["morph_mode"] = "authored"
            result["morph"] = "every authored timeline ratio plus both endpoints"
            result["morph_ratios"] = cache_key["morph_ratios"]
            result["morph_variants"] = sum(len(values) for values in morph_ratios.values())
            result["structure_sha256"] = cache_key["structure_sha256"]
        result["xml_sha256"] = cache_key["xml_sha256"]
        result["rasterizer_sha256"] = cache_key["ffdec_sha256"]
    args.output.with_suffix(".json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps({k: v for k, v in result.items() if k != "asset_list"}, indent=2))


if __name__ == "__main__":
    main()
