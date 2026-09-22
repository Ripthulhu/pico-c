#!/usr/bin/env python3
"""Validate shipped sound banks with a separate reader and IMA decoder."""

import hashlib
import json
import math
import mmap
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import build_audio as build

REC = struct.Struct("<HBBIIIIIII")
# Standard IMA step table, kept separate from the encoder under test.
STEP = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
    143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
    449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166,
    1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749,
    3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
)


def read_bank(data):
    if len(data) < 16:
        raise ValueError("Short header")
    magic, version, count, directory, size = struct.unpack_from("<4sHHII", data)
    if magic != b"PCTS" or version != 1 or not count or directory != 16 or size != len(data):
        raise ValueError("Invalid header")
    directory_end = directory + count * 32
    if directory_end > size:
        raise ValueError("Short directory")
    entries = []
    previous = 0
    for n in range(count):
        symbol, codec, channels, rate, frames, offset, length, block, r1, r2 = REC.unpack_from(data, directory + n * 32)
        if symbol <= previous or codec not in (0, 1) or channels not in (1, 2):
            raise ValueError("Invalid symbol or codec")
        previous = symbol
        if rate not in (11025, 22050, 44100) or not frames or r1 or r2:
            raise ValueError("Invalid stream properties")
        expected = frames * channels * 2 if codec == 0 else ((frames + 255) // 256) * channels * 132
        if length != expected or block != (256 if codec else 0):
            raise ValueError("Invalid payload length")
        if offset < directory_end or offset + length > size:
            raise ValueError("Payload outside bank")
        signature = (codec, channels, rate, frames, block)
        for old in entries:
            if offset < old["offset"] + old["length"] and old["offset"] < offset + length:
                if (offset, length, signature) != (old["offset"], old["length"], old["signature"]):
                    raise ValueError("Partial or incompatible payload overlap")
        payload = data[offset:offset + length]
        if codec == 1:
            for p in range(0, length, 132):
                if payload[p + 2] > 88 or payload[p + 3] or payload[p + 131] & 0xf0:
                    raise ValueError("Invalid IMA block header or padding")
        entries.append({"symbol": symbol, "codec": codec, "channels": channels, "rate": rate,
                        "frames": frames, "offset": offset, "length": length,
                        "signature": signature, "payload": payload})
    return entries


def decode_channel(block):
    if len(block) != 132:
        raise ValueError("Short IMA block")
    sample, index, reserved = struct.unpack_from("<hBB", block)
    if index > 88 or reserved or block[-1] & 0xf0:
        raise ValueError("Invalid IMA header or padding")
    output = [sample]
    for n in range(255):
        code = (block[4 + n // 2] >> ((n % 2) * 4)) & 15
        magnitude = code & 7
        step = STEP[index]
        change = step // 8
        for bit, divisor in ((1, 4), (2, 2), (4, 1)):
            if magnitude & bit:
                change += step // divisor
        sample += -change if code >= 8 else change
        sample = min(32767, max(-32768, sample))
        index += -1 if magnitude < 4 else 2 * (magnitude - 3)
        index = min(88, max(0, index))
        output.append(sample)
    return output


def decode_ima(entry):
    channels = entry["channels"]
    decoded = []
    for p in range(0, len(entry["payload"]), 132 * channels):
        planes = [decode_channel(entry["payload"][p + c * 132:p + (c + 1) * 132]) for c in range(channels)]
        decoded.extend(value for pair in zip(*planes) for value in pair)
    return decoded[:entry["frames"] * channels]


def entry(symbol=1, codec=0, frames=3, channels=1):
    pcm = struct.pack("<" + "h" * (frames * channels), *range(frames * channels))
    return {"symbol": symbol, "codec": codec, "channels": channels, "rate": 11025,
            "frames": frames, "payload": build.encode_ima(pcm, channels) if codec else pcm}


class AudioAssets(unittest.TestCase):
    def test_shipped_banks_and_manifests(self):
        for name, codec in (("pico_sound", 0), ("pico_sound_adpcm", 1), ("pico_sound_micro", 1)):
            with self.subTest(name=name):
                data = (ROOT / "assets" / f"{name}.pcts").read_bytes()
                manifest = json.loads((ROOT / "assets" / f"{name}.json").read_text())
                self.assertEqual(hashlib.sha256(data).hexdigest(), manifest["sha256"])
                self.assertEqual(len(data), manifest["bytes"])
                records = read_bank(data)
                self.assertEqual(len(records), 15)
                self.assertEqual(len(records), len(manifest["sounds"]))
                for record, item, source in zip(records, manifest["sounds"], manifest["sources"]):
                    self.assertEqual(record["symbol"], item["symbol"])
                    self.assertEqual(record["symbol"], source["symbol"])
                    self.assertEqual(record["codec"], codec)
                    for key in ("channels", "rate", "frames"):
                        self.assertEqual(record[key], item[key])
                    self.assertEqual(record["offset"], item["payload_offset"])
                    self.assertEqual(record["length"], item["payload_bytes"])
                    self.assertEqual(hashlib.sha256(record["payload"]).hexdigest(), item["payload_sha256"])
                    self.assertEqual(source["raw_pcm_frames"], source["seek_frames"] + source["swf_frames"] + source["discarded_tail_frames"])
                    if name == "pico_sound_micro":
                        self.assertEqual(record["channels"], 1)
                        self.assertEqual(record["rate"], 11025)
                        self.assertEqual(record["frames"], (source["swf_frames"] * 11025 + source["rate"] - 1) // source["rate"])
                        self.assertEqual(len(decode_ima(record)), record["frames"])
                    elif codec:
                        self.assertEqual(record["rate"], source["rate"])
                        self.assertEqual(record["channels"], source["channels"])
                        self.assertEqual(record["frames"], source["swf_frames"])
                        self.assertEqual(len(decode_ima(record)), record["frames"] * record["channels"])
                    else:
                        self.assertEqual(record["frames"], source["swf_frames"])
                        self.assertEqual(item["payload_sha256"], source["trimmed_pcm_sha256"])

    def test_source_inventory(self):
        xml = ROOT.parent / "pico-decomp/exports/movie.xml"
        if not xml.exists():
            self.skipTest("Recovered sources are not installed")
        manifest = json.loads((ROOT / "assets/pico_sound.json").read_text())
        self.assertEqual(hashlib.sha256(xml.read_bytes()).hexdigest(), manifest["source_xml_sha256"])
        sources = {int(e.get("soundId")): e for e in ET.parse(xml).getroot().iter() if e.get("type") == "DefineSoundTag"}
        self.assertEqual(set(sources), {s["symbol"] for s in manifest["sources"]})
        for item in manifest["sources"]:
            source = sources[item["symbol"]]
            data = bytes.fromhex(source.get("soundData"))
            self.assertEqual(struct.unpack_from("<h", data)[0], item["seek_frames"])
            self.assertEqual(int(source.get("soundSampleCount")), item["swf_frames"])
            self.assertEqual(hashlib.sha256(data[2:]).hexdigest(), item["source_sha256"])
            inventory = build.mp3_inventory(data[2:])
            for key in inventory:
                self.assertEqual(inventory[key], item[key])

    def test_ima_known_codes_and_clipping(self):
        block = struct.pack("<hBB", 0, 0, 0) + bytes([0x77, 0x77, 0x0f]) + bytes(125)
        self.assertEqual(decode_channel(block)[:7], [0, 11, 41, 104, 240, -53, -11])
        positive = struct.pack("<hBB", 32760, 88, 0) + bytes([7]) + bytes(127)
        negative = struct.pack("<hBB", -32760, 88, 0) + bytes([15]) + bytes(127)
        self.assertEqual(decode_channel(positive)[1], 32767)
        self.assertEqual(decode_channel(negative)[1], -32768)

    def test_ima_partial_blocks_and_stereo(self):
        for channels in (1, 2):
            values = [round(12000 * math.sin(i / 17)) * (1 if c == 0 else -1)
                      for i in range(517) for c in range(channels)]
            pcm = struct.pack("<" + "h" * len(values), *values)
            encoded = build.encode_ima(pcm, channels)
            decoded = decode_ima({"payload": encoded, "channels": channels, "frames": 517})
            self.assertEqual(len(encoded), 3 * channels * 132)
            self.assertEqual(len(values), len(decoded))
            for frame in (0, 256, 512):
                self.assertEqual(decoded[frame * channels:(frame + 1) * channels], values[frame * channels:(frame + 1) * channels])
            error = sum((a - b) ** 2 for a, b in zip(values, decoded)) / len(values)
            self.assertLess(math.sqrt(error), 500)

    def test_exact_payload_deduplication(self):
        first, second = entry(1), entry(2)
        bank, _ = build.build_bank([second, first])
        records = read_bank(bank)
        self.assertEqual([r["symbol"] for r in records], [1, 2])
        self.assertEqual(records[0]["offset"], records[1]["offset"])
        self.assertEqual(len(bank), 16 + 64 + 6)

    def test_reject_malformed_banks(self):
        bank, _ = build.build_bank([entry(1, codec=1)])
        mutations = [(0, b"FAIL"), (4, struct.pack("<H", 2)), (8, struct.pack("<I", 0)),
                     (12, struct.pack("<I", len(bank) - 1)), (18, b"\x02"), (19, b"\x03"),
                     (20, struct.pack("<I", 12345)), (24, bytes(4)), (28, struct.pack("<I", 16)),
                     (32, struct.pack("<I", 131)), (36, bytes(4)), (40, b"\x01"),
                     (50, b"\x59"), (51, b"\x01"), (len(bank) - 1, b"\xf0")]
        for offset, replacement in mutations:
            with self.subTest(offset=offset):
                bad = bytearray(bank)
                bad[offset:offset + len(replacement)] = replacement
                with self.assertRaises(ValueError):
                    read_bank(bad)
        with self.assertRaises(ValueError):
            read_bank(bank[:-1])
        different = entry(2)
        different["payload"] = bytes([1]) + different["payload"][1:]
        bank, _ = build.build_bank([entry(1), different])
        bad = bytearray(bank)
        struct.pack_into("<I", bad, 16 + 32 + 12, 81)
        with self.assertRaises(ValueError):
            read_bank(bad)

    def test_seek_is_applied_once_and_lengths_are_strict(self):
        pcm = struct.pack("<6h", 1, 2, 3, 4, 5, 6)
        self.assertEqual(build.trim_pcm(pcm, 2, 1, 2), struct.pack("<4h", 3, 4, 5, 6))
        self.assertEqual(build.trim_pcm(pcm, 1, -2, 4), struct.pack("<4h", 0, 0, 1, 2))
        with self.assertRaises(ValueError):
            build.trim_pcm(pcm, 1, 2, 5)

    def test_raw_mp3_frame_counts_reject_wrappers_and_truncation(self):
        frame = bytes.fromhex("fffb9000") + bytes(413)
        self.assertEqual(build.mp3_inventory(frame * 2),
                         {"mp3_frames": 2, "raw_pcm_frames": 2304, "rate": 44100, "channels": 2})
        for bad in (frame[:-1], b"ID3" + frame, frame[:36] + b"Xing" + frame[40:]):
            with self.assertRaises(ValueError):
                build.mp3_inventory(bad)

    @unittest.skipUnless(os.name == "posix", "Open mmap replacement is a POSIX host guarantee")
    def test_rebuild_keeps_existing_mapping_readable(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sound.pcts"
            path.write_bytes(b"old bank")
            with path.open("rb") as source, mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as mapped:
                build.write_atomic(path, b"new bank")
                self.assertEqual(mapped[:], b"old bank")
                self.assertEqual(path.read_bytes(), b"new bank")


if __name__ == "__main__":
    unittest.main()
