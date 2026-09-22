"""Check playback timing without opening a speaker or depending on a sound server."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import wave


def read_wav(path):
    with wave.open(str(path), "rb") as stream:
        assert stream.getsampwidth() == 2
        return stream.getparams(), stream.readframes(stream.getnframes())


def run(binary, root, work, name, *args):
    wav = work / (name + ".wav")
    command = [str(binary), "--assets", str(root / "assets/pico_art_micro.pcta"),
               "--width", "96", "--height", "64", "--root", "23",
               "--ticks", "97", "--render-every", "0", *map(str, args)]
    if name != "silent":
        command += ["--wav", str(wav)]
    result = subprocess.run(command, cwd=root, text=True, capture_output=True, check=True)
    metrics = json.loads(result.stdout)
    assert metrics["runtime_errors"] == 0, result.stdout
    assert metrics["sound_events"] > 0, result.stdout
    if name == "silent":
        assert metrics["sound_events_silent"] == metrics["sound_events"]
        return metrics, None, None
    assert metrics["sound_events_silent"] == 0, result.stdout
    assert "0 dropped cues, 0 missing sounds" in result.stderr, result.stderr
    params, data = read_wav(wav)
    assert params.nframes == 97 * params.framerate // 24, params
    assert any(data), name
    return metrics, params, data


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    args.binary, args.root = args.binary.resolve(), args.root.resolve()
    with tempfile.TemporaryDirectory(prefix="pico-audio-") as folder:
        work = Path(folder)
        silent, _, _ = run(args.binary, args.root, work, "silent")
        for filename in ("pico_sound.pcts", "pico_sound_micro.pcts", "pico_sound_adpcm.pcts"):
            path = args.root / "assets" / filename
            first, params, data = run(args.binary, args.root, work, filename, "--sounds", path)
            second, params2, data2 = run(args.binary, args.root, work, filename + "-draw",
                                         "--sounds", path, "--render-every", "13")
            assert params == params2 and data == data2, "Drawing cadence changed audio"
            for key in ("frame_hash_fnv1a", "root_frame", "actions_executed", "sound_events"):
                assert first[key] == second[key] == silent[key], key

        # The ending starts music during pico_init. It must reach the sink
        # before the first audio block, even though host_open clears HostApp.
        symbols = (52, 201, 214, 215, 218, 223, 242, 268, 316, 321, 405, 759, 825, 900, 992)
        payload = struct.pack("<h", 12000) * (11025 * 5)
        offset = 16 + 32 * len(symbols)
        bank = bytearray(struct.pack("<4sHHII", b"PCTS", 1, len(symbols), 16, offset + len(payload)))
        for symbol in symbols:
            bank += struct.pack("<HBBIIIIIII", symbol, 0, 1, 11025, len(payload) // 2,
                                offset, len(payload), 0, 0, 0)
        bank += payload
        fixture = work / "constant.pcts"
        fixture.write_bytes(bank)
        _, _, samples = run(args.binary, args.root, work, "startup", "--sounds", fixture,
                             "--root", "22")
        assert struct.unpack_from("<h", samples)[0] != 0, "Lost initial sound cue"

        # Use disposable copies for alias checks. An unsafe implementation must
        # never get a chance to truncate the checked-in artwork or sound banks.
        art_fixture = work / "art.pcta"
        art_bytes = (args.root / "assets/pico_art_micro.pcta").read_bytes()
        art_fixture.write_bytes(art_bytes)
        for source in (fixture, art_fixture):
            hard_link = work / (source.name + ".hardlink")
            os.link(source, hard_link)
            for output in (source, hard_link):
                rejected = subprocess.run(
                    [str(args.binary), "--assets", str(art_fixture), "--sounds", str(fixture),
                     "--wav", str(output), "--ticks", "1", "--render-every", "0"],
                    cwd=args.root, capture_output=True)
                assert rejected.returncode == 2, "WAV output must reject an input alias"
                assert fixture.read_bytes() == bank, "Sound bank changed after alias rejection"
                assert art_fixture.read_bytes() == art_bytes, "Artwork changed after alias rejection"

        existing = work / "overwrite.wav"
        existing.write_bytes(bytes(2 * 1024 * 1024))
        _, params, data = run(args.binary, args.root, work, "overwrite", "--sounds", fixture)
        assert existing.stat().st_size == 44 + len(data), "Old WAV trailing bytes remain"

        broken = work / "broken.pcts"
        broken.write_bytes(bank[:50])
        failed = subprocess.run([str(args.binary), "--assets", str(args.root / "assets/pico_art_micro.pcta"),
                                 "--sounds", str(broken), "--wav", str(work / "bad.wav")],
                                cwd=args.root, capture_output=True)
        assert failed.returncode == 2, "Invalid pack must fail WAV export"
    print("Audio host: three packs, timing, unchanged gameplay, startup cues and input alias protection pass.")


if __name__ == "__main__":
    main()
