#!/usr/bin/env python3
"""Check shipped art packs, with extra source checks when available."""

import hashlib
import json
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
DECOMP = ROOT.parent / "pico-decomp"
RECORD = struct.Struct("<HHhhHHIII")
PACK_SCALES = {
    "pico_art": (2, 5),
    "pico_art_micro": (1, 5),
    "pico_art_full": (1, 1),
    "pico_art_rgba": (1, 1),
    "pico_art_rgba_2x": (2, 1),
}
sys.path.insert(0, str(ROOT / "tools"))
try:
    import build_assets as build
except ModuleNotFoundError as error:
    if error.name not in {"PIL", "PIL.Image"}:
        raise
    build = None


def decode_lz4(data, expected):
    output = bytearray()
    p = 0
    final_literals = False
    while p < len(data):
        token = data[p]
        p += 1
        literal_size = token >> 4
        if literal_size == 15:
            while True:
                if p >= len(data):
                    raise ValueError("Truncated LZ4 literal length")
                extra = data[p]
                p += 1
                literal_size += extra
                if extra != 255:
                    break
        if p + literal_size > len(data) or len(output) + literal_size > expected:
            raise ValueError("LZ4 literal exceeds row")
        output.extend(data[p : p + literal_size])
        p += literal_size
        if p == len(data):
            final_literals = True
            break
        if p + 2 > len(data):
            raise ValueError("Truncated LZ4 offset")
        offset = data[p] | data[p + 1] << 8
        p += 2
        if not offset or offset > len(output):
            raise ValueError("Invalid LZ4 offset")
        match_size = (token & 15) + 4
        if (token & 15) == 15:
            while True:
                if p >= len(data):
                    raise ValueError("Truncated LZ4 match length")
                extra = data[p]
                p += 1
                match_size += extra
                if extra != 255:
                    break
        if len(output) + match_size > expected:
            raise ValueError("LZ4 match exceeds row")
        pattern = output[-offset:]
        output.extend((pattern * ((match_size + offset - 1) // offset))[:match_size])
    if not final_literals or len(output) != expected:
        raise ValueError("Incomplete LZ4 row")
    return output


def decode_rgba_row(data, width):
    if not data:
        raise ValueError("Missing row mode")
    mode, payload = data[0], data[1:]
    size = width * 4
    if mode == 0:
        result = bytearray(payload)
    elif mode == 1:
        if len(payload) % 5:
            raise ValueError("Truncated RGBA run")
        result = bytearray()
        for p in range(0, len(payload), 5):
            if not payload[p] or len(result) + payload[p] * 4 > size:
                raise ValueError("Invalid RGBA run")
            result.extend(payload[p + 1 : p + 5] * payload[p])
    elif 2 <= mode <= 5:
        result = decode_lz4(payload, size)
        if mode in (3, 5):
            interleaved = bytearray(size)
            for channel in range(4):
                interleaved[channel::4] = result[channel * width : (channel + 1) * width]
            result = interleaved
        if mode in (4, 5):
            for p in range(4, size):
                result[p] = (result[p] + result[p - 4]) & 255
    else:
        raise ValueError("Unknown row mode")
    if len(result) != size:
        raise ValueError("Incomplete RGBA row")
    return result


class AssetsTest(unittest.TestCase):
    def test_lz4_decoder(self):
        self.assertEqual(decode_lz4(b"\x50hello", 5), b"hello")
        self.assertEqual(decode_lz4(b"\x13a\x01\x00\x50hello", 13), b"aaaaaaaahello")
        for data, size in ((b"\xf0", 10), (b"\x10a\x00\x00", 10), (b"\x50hello", 4)):
            with self.assertRaises(ValueError):
                decode_lz4(data, size)

    def test_rgba_modes(self):
        expected = bytes((250, 1, 2, 100, 10, 3, 2, 130))
        rows = (
            b"\x00" + expected,
            bytes((1, 1, 250, 1, 2, 100, 1, 10, 3, 2, 130)),
            b"\x02\x80" + expected,
            bytes((3, 128, 250, 10, 1, 3, 2, 2, 100, 130)),
            bytes((4, 128, 250, 1, 2, 100, 16, 2, 0, 30)),
            bytes((5, 128, 250, 16, 1, 2, 2, 0, 100, 30)),
        )
        for row in rows:
            self.assertEqual(decode_rgba_row(row, 2), expected)
        for data in (b"", b"\xff", b"\x00\x01", b"\x01\x00\x01\x02\x03\x04"):
            with self.assertRaises(ValueError):
                decode_rgba_row(data, 1)

    @unittest.skipIf(build is None, "Pillow absent; skipping the asset compiler check")
    def test_row_encoder(self):
        for data in (b"", bytes([3]) * 700, bytes(range(256)), b"\x00\x00\x01\x01\x00"):
            encoded = build.rle_row(data)
            self.assertTrue(all(encoded[i] > 0 for i in range(0, len(encoded), 2)))
            decoded = b"".join(
                bytes([encoded[i + 1]]) * encoded[i] for i in range(0, len(encoded), 2)
            )
            self.assertEqual(data, decoded)

    @unittest.skipIf(build is None, "Pillow absent; skipping the asset compiler check")
    def test_rgba_row_encoder(self):
        rows = (
            b"",
            bytes((1, 2, 3, 0)) * 700,
            bytes(channel for alpha in range(256) for channel in (17, 111, 255, alpha)),
            bytes((0, 0, 0, 255, 0, 0, 1, 255, 0, 0, 1, 128, 0, 0, 1, 0)),
        )
        for data in rows:
            encoded = build.rle_rgba_row(data)
            self.assertEqual(len(encoded) % 5, 0)
            self.assertTrue(all(encoded[i] > 0 for i in range(0, len(encoded), 5)))
            decoded = b"".join(
                encoded[i + 1 : i + 5] * encoded[i] for i in range(0, len(encoded), 5)
            )
            self.assertEqual(data, decoded)
        with self.assertRaises(ValueError):
            build.rle_rgba_row(b"\x01\x02\x03")

    def test_complete_packs(self):
        for name, scale in PACK_SCALES.items():
            with self.subTest(profile=name):
                path = ROOT / "assets" / f"{name}.pcta"
                data = path.read_bytes()
                report = json.loads(path.with_suffix(".json").read_text())
                rgba = name.startswith("pico_art_rgba")
                self.assertEqual(data[:4], b"PCTA")
                version, count, num, den, records, palette = struct.unpack_from("<HHHHII", data, 4)
                self.assertEqual(version, 3 if rgba else 1)
                self.assertEqual(count, 703 if rgba else 712)
                self.assertEqual((num, den), scale)
                self.assertEqual(report["raster_scale"], list(scale))
                self.assertEqual(report["asset_records"], count)
                self.assertEqual(report["pack_bytes"], len(data))
                self.assertEqual(report["pack_sha256"], hashlib.sha256(data).hexdigest())
                if rgba:
                    self.assertEqual(palette, 0)
                    self.assertEqual(report["palette_colors"], 0)
                    self.assertEqual(report["capture_scale"], list(scale))
                    self.assertEqual(report["morph_mode"], "authored")
                    self.assertEqual(report["morph_variants"], 36)
                    self.assertEqual(sum(len(ratios) for ratios in report["morph_ratios"].values()), 36)
                else:
                    self.assertLess(palette + 512, len(data))
                    self.assertEqual(data[palette : palette + 2], b"\0\0")
                symbols = set()
                keys = []
                pixels = 0
                seen_tables = set()
                row_modes = [0] * 6
                for i in range(count):
                    symbol, ratio, ox, oy, w, h, rows, start, end = RECORD.unpack_from(
                        data, records + i * 24
                    )
                    symbols.add(symbol)
                    keys.append((symbol, ratio))
                    pixels += w * h
                    self.assertGreater(w, 0)
                    self.assertGreater(h, 0)
                    self.assertLessEqual(rows + (h + 1) * 4, len(data))
                    offsets = struct.unpack_from(f"<{h+1}I", data, rows)
                    self.assertEqual(offsets[0], start)
                    self.assertEqual(offsets[-1], end)
                    self.assertLessEqual(end, len(data))
                    digest = hashlib.sha256()
                    for left, right in zip(offsets, offsets[1:]):
                        self.assertLess(left, right)
                        if rgba:
                            digest.update(decode_rgba_row(data[left:right], w))
                            if rows not in seen_tables:
                                row_modes[data[left]] += 1
                        else:
                            self.assertEqual((right - left) % 2, 0)
                            lengths = data[left:right:2]
                            self.assertTrue(all(n > 0 for n in lengths))
                            self.assertEqual(sum(lengths), w)
                    seen_tables.add(rows)
                    if rgba:
                        self.assertEqual(digest.hexdigest(), report["asset_list"][i]["rgba_sha256"])
                self.assertEqual(symbols, {asset["symbol"] for asset in report["asset_list"]})
                self.assertEqual(
                    keys, [(asset["symbol"], asset["ratio"]) for asset in report["asset_list"]]
                )
                self.assertEqual(keys, sorted(set(keys)))
                if rgba:
                    self.assertEqual(report["row_modes"], {str(i): n for i, n in enumerate(row_modes)})
                    self.assertTrue(all(n > 0 for n in row_modes))
                    for symbol, ratios in report["morph_ratios"].items():
                        self.assertEqual([ratio for id_, ratio in keys if id_ == int(symbol)], ratios)
                self.assertEqual(
                    pixels, report["uncompressed_rgba_pixels" if rgba else "uncompressed_index8_pixels"]
                )
                if rgba:
                    self.assertEqual(pixels * 4, report["uncompressed_rgba_bytes"])

    @unittest.skipUnless(
        build is not None and (DECOMP / "exports/movie.xml").is_file(),
        "Decomp reference or Pillow absent; skipping the source inventory check",
    )
    def test_source_inventory(self):
        expected = {
            entry["symbol"] for entry in build.leaf_metadata(DECOMP / "exports/movie.xml", 2, 5, 9)
        }
        expected.update(
            int(path.stem.split("_")[0]) for path in (DECOMP / "exports/images").glob("*.png")
        )
        for name, scale in PACK_SCALES.items():
            report = json.loads((ROOT / "assets" / f"{name}.json").read_text())
            self.assertEqual({asset["symbol"] for asset in report["asset_list"]}, expected)
            morph_ratios = None
            if name.startswith("pico_art_rgba"):
                morph_ratios = {int(symbol): ratios for symbol, ratios in report["morph_ratios"].items()}
                if (DECOMP / "analysis/structure.json").exists():
                    self.assertEqual(
                        morph_ratios, build.authored_morph_ratios(DECOMP / "analysis/structure.json")
                    )
            leaves = build.leaf_metadata(DECOMP / "exports/movie.xml", *scale, 9, morph_ratios)
            expected_keys = {(leaf["symbol"], leaf["ratio"]) for leaf in leaves}
            expected_keys.update(
                (int(path.stem.split("_")[0]), 0) for path in (DECOMP / "exports/images").glob("*.png")
            )
            self.assertEqual(
                {(asset["symbol"], asset["ratio"]) for asset in report["asset_list"]}, expected_keys
            )

    @unittest.skipIf(build is None, "Pillow absent; skipping the asset compiler check")
    def test_temporary_wrapper_has_no_scripts(self):
        for profile in ("assets", "assets-micro", "assets-full", "assets-rgba", "rgba-native"):
            path = ROOT / "build" / profile / "leaves.swf"
            if not path.exists():
                continue
            tags = list(build.source_tags(path))
            codes = [code for code, body, raw in tags]
            self.assertEqual(codes.count(1), 696 if profile in ("assets-rgba", "rgba-native") else 705)
            self.assertTrue(set(codes).isdisjoint({7, 12, 14, 15, 18, 19, 34, 39, 45, 59, 82}))

    @unittest.skipIf(build is None, "Pillow absent; skipping the source pixel check")
    def test_source_pixels(self):
        checked = 0
        for name, work in (("pico_art_rgba", "rgba-native"), ("pico_art_rgba_2x", "assets-rgba")):
            cache = ROOT / "build" / work
            if not (cache / "leaves.json").exists():
                continue
            source = {
                (leaf["symbol"], leaf["ratio"]): leaf
                for leaf in json.loads((cache / "leaves.json").read_text())
            }
            report = json.loads((ROOT / "assets" / f"{name}.json").read_text())
            for asset in report["asset_list"]:
                if asset["kind"] == "bitmap":
                    paths = list((DECOMP / "exports/images").glob(f"{asset['symbol']}_*.png"))
                    if not paths:
                        continue
                    self.assertEqual(len(paths), 1)
                    with build.Image.open(paths[0]) as image:
                        pixels = image.convert("RGBA").resize(
                            tuple(asset["size"]), build.Image.Resampling.LANCZOS
                        ).tobytes()
                    self.assertEqual(hashlib.sha256(pixels).hexdigest(), asset["rgba_sha256"])
                    checked += 1
                    continue
                original = source[asset["symbol"], asset["ratio"]]
                path = cache / "rendered/frames" / f"{original['frame']}.png"
                if not path.exists():
                    path = cache / "rendered" / f"{original['frame']}.png"
                dx = asset["origin"][0] - original["origin"][0]
                dy = asset["origin"][1] - original["origin"][1]
                with build.Image.open(path) as image:
                    pixels = image.convert("RGBA").crop(
                        (dx, dy, dx + asset["size"][0], dy + asset["size"][1])
                    ).tobytes()
                self.assertEqual(hashlib.sha256(pixels).hexdigest(), asset["rgba_sha256"])
                checked += 1
        if not checked:
            self.skipTest("No cached source renders; skipping the source pixel check")


if __name__ == "__main__":
    unittest.main()
