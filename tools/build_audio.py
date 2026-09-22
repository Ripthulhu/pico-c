#!/usr/bin/env python3
"""Build seek-corrected PCM and IMA sound banks from recovered MP3s."""

from __future__ import annotations

import argparse
from array import array
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
DECOMP = ROOT.parent / "pico-decomp"
HEADER = struct.Struct("<4sHHII")
RECORD = struct.Struct("<HBBIIIIIII")
BLOCK_FRAMES = 256
CHANNEL_BYTES = 132
INDEX = (-1, -1, -1, -1, 2, 4, 6, 8)
STEPS = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
    143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
    449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166,
    1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749,
    3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def write_atomic(path, data):
    # A running host may have the old bank mapped. Never truncate its inode.
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(data)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def samples_from_bytes(data):
    if len(data) % 2:
        raise ValueError("PCM has an incomplete sample")
    values = array("h")
    values.frombytes(data)
    if sys.byteorder != "little":
        values.byteswap()
    return values


def mp3_inventory(data):
    """Count every raw Layer III frame; reject metadata and changing formats.

    Comparing this count with decoder output catches automatic delay trimming.
    The recovered files contain raw MPEG frames with no ID3/Xing wrapper.
    """
    pos = frames = decoded_frames = 0
    format_seen = None
    while pos < len(data):
        if len(data) - pos < 4:
            raise ValueError("Truncated MP3 header")
        h = int.from_bytes(data[pos:pos + 4], "big")
        version = (h >> 19) & 3
        layer = (h >> 17) & 3
        bitrate_index = (h >> 12) & 15
        rate_index = (h >> 10) & 3
        if (h >> 21 != 0x7ff or version == 1 or layer != 1 or
                bitrate_index in (0, 15) or rate_index == 3):
            raise ValueError(f"Unsupported MP3 frame at byte {pos}")
        rate = (44100, 48000, 32000)[rate_index] // {3: 1, 2: 2, 0: 4}[version]
        channels = 1 if (h >> 6) & 3 == 3 else 2
        current_format = (rate, channels)
        if format_seen is not None and current_format != format_seen:
            raise ValueError("MP3 changes rate or channel count")
        format_seen = current_format
        bitrates = ((0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320)
                    if version == 3 else
                    (0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160))
        length = (144000 if version == 3 else 72000) * bitrates[bitrate_index] // rate
        length += (h >> 9) & 1
        if pos + length > len(data):
            raise ValueError("Truncated MP3 frame")
        if frames == 0:
            crc_bytes = 0 if h & (1 << 16) else 2
            side_bytes = (17 if channels == 1 else 32) if version == 3 else (9 if channels == 1 else 17)
            xing = pos + 4 + crc_bytes + side_bytes
            if data[xing:xing + 4] in (b"Xing", b"Info") or data[pos + 36:pos + 40] == b"VBRI":
                raise ValueError("MP3 gapless metadata needs a separate trim policy")
        pos += length
        frames += 1
        decoded_frames += 1152 if version == 3 else 576
    if not frames:
        raise ValueError("Empty MP3")
    return {"mp3_frames": frames, "raw_pcm_frames": decoded_frames,
            "rate": format_seen[0], "channels": format_seen[1]}


def read_sources(xml_path, sound_dir):
    result = []
    seen = set()
    for element in ET.parse(xml_path).getroot().iter():
        if element.get("type") != "DefineSoundTag":
            continue
        symbol = int(element.get("soundId"))
        if symbol in seen or not 0 < symbol < 65536:
            raise ValueError("Duplicate or invalid sound symbol")
        seen.add(symbol)
        if int(element.get("soundFormat")) != 2:
            raise ValueError("This conversion expects MP3 DefineSound tags")
        body = bytes.fromhex(element.get("soundData"))
        seek = struct.unpack_from("<h", body)[0]
        source = (sound_dir / f"{symbol}.mp3").read_bytes()
        if source != body[2:]:
            raise ValueError(f"Export does not match DefineSound {symbol}")
        info = mp3_inventory(source)
        rate = (5512, 11025, 22050, 44100)[int(element.get("soundRate"))]
        channels = 2 if element.get("soundType") == "true" else 1
        if (info["rate"], info["channels"]) != (rate, channels):
            raise ValueError(f"SWF and MP3 formats disagree for {symbol}")
        result.append({"symbol": symbol, "source_file": f"{symbol}.mp3",
                       "source_bytes": len(source), "source_sha256": sha256(source),
                       "seek_frames": seek,
                       "swf_frames": int(element.get("soundSampleCount")),
                       **info, "data": source})
    if not result:
        raise ValueError("No DefineSound tags")
    return sorted(result, key=lambda item: item["symbol"])


def ffmpeg_output(ffmpeg, args, data):
    command = [ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", *args]
    result = subprocess.run(command, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise RuntimeError(result.stderr.decode("utf-8", "replace"))
    return result.stdout


def trim_pcm(pcm, channels, seek, count):
    width = channels * 2
    if len(pcm) % width or count <= 0:
        raise ValueError("Invalid PCM or SWF sample count")
    if seek < 0:
        pcm = bytes(-seek * width) + pcm
        seek = 0
    end = (seek + count) * width
    if end > len(pcm):
        raise ValueError("Decoded sound is shorter than SWF seek plus sample count")
    return pcm[seek * width:end]


def decode_source(ffmpeg, source):
    pcm = ffmpeg_output(ffmpeg, ["-flags2", "+skip_manual", "-c:a", "mp3", "-f", "mp3",
                                "-i", "pipe:0", "-map_metadata", "-1", "-bitexact",
                                "-c:a", "pcm_s16le", "-f", "s16le", "pipe:1"], source["data"])
    expected = source["raw_pcm_frames"] * source["channels"] * 2
    if len(pcm) != expected:
        raise ValueError(f"Decoder returned {len(pcm)} bytes, expected {expected}; delay handling changed")
    return trim_pcm(pcm, source["channels"], source["seek_frames"], source["swf_frames"])


def micro_pcm(ffmpeg, pcm, rate, channels):
    filters = []
    if channels == 2:
        filters.append("pan=mono|c0=0.5*c0+0.5*c1")
    filters.append("aresample=11025:resampler=swr:dither_method=none")
    converted = ffmpeg_output(ffmpeg, ["-f", "s16le", "-ar", str(rate), "-ac", str(channels),
                                      "-i", "pipe:0", "-af", ",".join(filters), "-bitexact",
                                      "-c:a", "pcm_s16le", "-f", "s16le", "pipe:1"], pcm)
    expected = (len(pcm) // (2 * channels) * 11025 + rate - 1) // rate
    short_frames = expected - len(converted) // 2
    if len(converted) % 2 or short_frames not in (0, 1):
        raise ValueError("Resampler changed the expected frame count by more than rounding")
    return converted + bytes(short_frames * 2), short_frames


def ima_nibble(predictor, index, code):
    step = STEPS[index]
    difference = step >> 3
    if code & 1:
        difference += step >> 2
    if code & 2:
        difference += step >> 1
    if code & 4:
        difference += step
    predictor += -difference if code & 8 else difference
    predictor = max(-32768, min(32767, predictor))
    index = max(0, min(88, index + INDEX[code & 7]))
    return predictor, index


def ima_channel(samples):
    samples = list(samples)
    if not 1 <= len(samples) <= BLOCK_FRAMES:
        raise ValueError("IMA block must have 1 to 256 frames")
    samples += [samples[-1]] * (BLOCK_FRAMES - len(samples))
    predictor = samples[0]
    # Seed each independently seekable block from its first few sample deltas.
    delta = sum(abs(samples[i] - samples[i - 1]) for i in range(1, 17)) // 16
    index = min(range(89), key=lambda i: abs(STEPS[i] - delta))
    output = bytearray(struct.pack("<hBB", predictor, index, 0) + bytes(128))
    for n, sample in enumerate(samples[1:]):
        difference = sample - predictor
        code = 8 if difference < 0 else 0
        difference = abs(difference)
        step = STEPS[index]
        for flag, threshold in ((4, step), (2, step >> 1), (1, step >> 2)):
            if difference >= threshold:
                code |= flag
                difference -= threshold
        predictor, index = ima_nibble(predictor, index, code)
        output[4 + n // 2] |= code << (4 * (n & 1))
    return bytes(output)


def encode_ima(pcm, channels):
    samples = samples_from_bytes(pcm)
    if channels not in (1, 2) or not samples or len(samples) % channels:
        raise ValueError("Invalid IMA input")
    frames = len(samples) // channels
    output = bytearray()
    for start in range(0, frames, BLOCK_FRAMES):
        end = min(frames, start + BLOCK_FRAMES)
        for channel in range(channels):
            output += ima_channel(samples[start * channels + channel:end * channels:channels])
    return bytes(output)


def build_bank(entries):
    if not entries or len(entries) > 65535:
        raise ValueError("Invalid bank entry count")
    output = bytearray(HEADER.size + RECORD.size * len(entries))
    records = []
    payloads = {}
    previous = 0
    for entry in sorted(entries, key=lambda item: item["symbol"]):
        symbol = entry["symbol"]
        if not previous < symbol < 65536:
            raise ValueError("Duplicate or invalid bank symbol")
        previous = symbol
        data = entry["payload"]
        key = (entry["codec"], entry["channels"], entry["rate"], entry["frames"], data)
        if key in payloads:
            offset, shared_symbol = payloads[key]
        else:
            offset, shared_symbol = len(output), None
            payloads[key] = (offset, symbol)
            output += data
        record = (symbol, entry["codec"], entry["channels"], entry["rate"], entry["frames"],
                  offset, len(data), BLOCK_FRAMES if entry["codec"] == 1 else 0, 0, 0)
        RECORD.pack_into(output, HEADER.size + len(records) * RECORD.size, *record)
        records.append({"symbol": symbol, "codec": "ima_adpcm" if entry["codec"] else "pcm_s16le",
                        "channels": entry["channels"], "rate": entry["rate"], "frames": entry["frames"],
                        "payload_offset": offset, "payload_bytes": len(data),
                        "payload_sha256": sha256(data), "shared_with": shared_symbol})
    HEADER.pack_into(output, 0, b"PCTS", 1, len(entries), HEADER.size, len(output))
    return bytes(output), records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--xml", type=Path, default=DECOMP / "exports/movie.xml")
    parser.add_argument("--sounds", type=Path, default=DECOMP / "exports/sounds")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "assets")
    args = parser.parse_args()
    ffmpeg_version = subprocess.check_output([args.ffmpeg, "-version"], text=True).splitlines()[0]
    sources = read_sources(args.xml, args.sounds)
    reference, adpcm, micro, inventory = [], [], [], []
    decoded = {}
    for source in sources:
        key = (source["source_sha256"], source["seek_frames"], source["swf_frames"])
        if key not in decoded:
            pcm = decode_source(args.ffmpeg, source)
            reduced, padded = micro_pcm(args.ffmpeg, pcm, source["rate"], source["channels"])
            decoded[key] = (pcm, reduced, encode_ima(reduced, 1), padded,
                            encode_ima(pcm, source["channels"]))
        pcm, reduced, encoded, padded, full_encoded = decoded[key]
        reference.append({"symbol": source["symbol"], "codec": 0, "channels": source["channels"],
                          "rate": source["rate"], "frames": source["swf_frames"], "payload": pcm})
        adpcm.append({"symbol": source["symbol"], "codec": 1, "channels": source["channels"],
                      "rate": source["rate"], "frames": source["swf_frames"], "payload": full_encoded})
        micro.append({"symbol": source["symbol"], "codec": 1, "channels": 1,
                      "rate": 11025, "frames": len(reduced) // 2, "payload": encoded})
        inventory.append({**{k: v for k, v in source.items() if k != "data"},
                          "trimmed_pcm_sha256": sha256(pcm),
                          "micro_input_pcm_sha256": sha256(reduced),
                          "micro_tail_zero_frames": padded,
                          "discarded_tail_frames": source["raw_pcm_frames"] - source["seek_frames"] - source["swf_frames"]})
        print(f"sound {source['symbol']}: {source['swf_frames']} frames at {source['rate']} Hz; {len(encoded)} small bytes", flush=True)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for filename, entries, profile in (("pico_sound.pcts", reference, "reference"),
                                       ("pico_sound_adpcm.pcts", adpcm, "adpcm"),
                                       ("pico_sound_micro.pcts", micro, "micro")):
        data, records = build_bank(entries)
        manifest = {"format": "PCTS", "version": 1, "profile": profile, "file": filename,
                    "bytes": len(data), "sha256": sha256(data), "ffmpeg": ffmpeg_version,
                    "source_xml_sha256": sha256(args.xml.read_bytes()),
                    "decode": "FFmpeg fixed-point mp3 decoder, skip_manual, raw frame count checked; apply signed SWF SeekSamples once, then SoundSampleCount",
                    "micro_conversion": "After source trim: average stereo channels, swresample 11025 Hz without dithering, ceil duration (at most one terminal zero sample), independent 256-frame IMA blocks",
                    "adpcm_conversion": "Source rates and channels retained; independent 256-frame IMA blocks",
                    "limitations": ["MP3 decoding is not compared with the original Flash decoder",
                                    "Both ADPCM banks use lossy compression; the micro bank also reduces bandwidth"],
                    "unique_payloads": len({record["payload_offset"] for record in records}),
                    "sounds": records, "sources": inventory}
        write_atomic(args.output_dir / filename, data)
        write_atomic((args.output_dir / filename).with_suffix(".json"),
                     (json.dumps(manifest, indent=2) + "\n").encode("utf-8"))
        print(f"{filename}: {len(data)} bytes, {manifest['unique_payloads']} unique payloads")


if __name__ == "__main__":
    main()
