"""Exercise the real host boundaries and deterministic authored game replay."""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    args = parser.parse_args()
    binary, assets = args.binary.resolve(), args.assets.resolve()
    with tempfile.TemporaryDirectory(prefix="pico-host-test-") as directory:
        temporary = Path(directory)

        def run(*options):
            result = subprocess.run(
                [
                    str(binary),
                    "--assets",
                    str(assets),
                    "--width",
                    "96",
                    "--height",
                    "64",
                    *map(str, options),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            metrics = json.loads(result.stdout)
            assert metrics["runtime_errors"] == metrics["draw_overflows"] == 0, metrics
            return metrics

        # An unspecified root must retain the original menu while waiting for
        # Play, including on the CLI used to bring up new device adapters.
        startup_state = temporary / "startup.json"
        startup = run("--ticks", 72, "--assert-root", 1, "--state", startup_state)
        assert startup["root_frame"] == 1
        instances = json.loads(startup_state.read_text())
        root = next(instance for instance in instances if instance["symbol"] == 0)
        loader = next(instance for instance in instances if instance["symbol"] == 22)
        assert root["frame"] == 1 and not root["playing"]
        assert loader["frame"] == 3 and not loader["playing"]
        assert {15, 21}.issubset({instance["symbol"] for instance in instances})
        assert all(instance["path"] != "_root/health" for instance in instances)

        # Identical byte input gives identical state and pixels despite host timing.
        trace = temporary / "hall.trace"
        trace.write_text("24 tap 84\n", encoding="ascii")
        image_a, image_b = temporary / "a.ppm", temporary / "b.ppm"
        state_a, state_b = temporary / "a.json", temporary / "b.json"
        a = run(
            "--root",
            2,
            "--ticks",
            48,
            "--trace",
            trace,
            "--assert-root",
            4,
            "--ppm",
            image_a,
            "--state",
            state_a,
        )
        b = run(
            "--root",
            2,
            "--ticks",
            48,
            "--trace",
            trace,
            "--assert-root",
            4,
            "--ppm",
            image_b,
            "--state",
            state_b,
        )
        assert image_a.read_bytes() == image_b.read_bytes()
        assert json.loads(state_a.read_text()) == json.loads(state_b.read_text())
        assert a["frame_hash_fnv1a"] == b["frame_hash_fnv1a"]
        assert a["root_frame"] == 4
        with assets.open("rb") as asset_file:
            version = int.from_bytes(asset_file.read(6)[4:6], "little")
        assert a["asset_pack_version"] == version
        assert a["host_pixel_format"] == ("xrgb8888" if version >= 2 else "rgb565")
        assert a["host_framebuffer_bytes"] == 96 * 64 * (4 if version >= 2 else 2)
        assert 0 <= a["asset_cache_used_bytes"] <= a["asset_cache_capacity_bytes"]
        if version != 3 or a["draw_list_capacity_bytes"] == 0:
            assert (
                a["asset_cache_capacity_bytes"] == a["asset_cache_metadata_bytes"] == 0
            )
        elif a["asset_cache_capacity_bytes"]:
            assert a["asset_cache_metadata_bytes"] > 0
        colour_pixels = image_a.read_bytes()[len(b"P6\n96 64\n255\n") :]
        red_blue = {i * 255 // 31 for i in range(32)}
        green = {i * 255 // 63 for i in range(64)}
        exceeds_rgb565 = any(
            colour_pixels[i] not in red_blue
            or colour_pixels[i + 1] not in green
            or colour_pixels[i + 2] not in red_blue
            for i in range(0, len(colour_pixels), 3)
        )
        assert exceeds_rgb565 == (
            version >= 2
        ), "PPM colour depth doesn't match the pack"
        if version >= 2:
            # The display hash uses defined little-endian XRGB bytes, independent
            # of the host's byte order and the pack's compression method.
            expected_hash = 2166136261
            for i in range(0, len(colour_pixels), 3):
                red, green_value, blue = colour_pixels[i : i + 3]
                for value in (blue, green_value, red, 0):
                    expected_hash = ((expected_hash ^ value) * 16777619) & 0xFFFFFFFF
            assert a["frame_hash_fnv1a"] == f"{expected_hash:08x}"

        # Packed monochrome output must contain only exact black/white samples.
        mono = temporary / "mono.ppm"
        packed = temporary / "mono.pbm"
        run("--root", 2, "--ticks", 24, "--mono", "--ppm", mono, "--pbm", packed)
        ppm_header = b"P6\n96 64\n255\n"
        data = mono.read_bytes()
        assert data.startswith(ppm_header)
        pixels = data[len(ppm_header) :]
        assert len(pixels) == 96 * 64 * 3
        assert set(pixels) == {0, 255}
        pbm_header = b"P4\n96 64\n"
        pbm = packed.read_bytes()
        assert pbm.startswith(pbm_header) and len(pbm) == len(pbm_header) + 768
        bits = pbm[len(pbm_header) :]
        for index in range(96 * 64):
            black = bool(bits[index // 8] & (0x80 >> (index & 7)))
            assert black == (pixels[index * 3] == 0)

        # Reader is a separate high contrast view of recovered original text.
        reader = temporary / "reader.ppm"
        run("--root", 2, "--ticks", 24, "--read-text", "--ppm", reader)
        assert reader.read_bytes() != data
        assert set(reader.read_bytes()[len(ppm_header) :]) == {0, 255}

        # Render every authored root scene after entry, including intro/endings.
        peak_instances = peak_draws = 0
        for root in range(2, 24):
            metrics = run("--root", root, "--ticks", 48, "--render-every", 12)
            peak_instances = max(peak_instances, metrics["peak_instances"])
            peak_draws = max(peak_draws, metrics["peak_draws"])
        print(
            f"Host regression passed: default menu, replay, images, reader, 22 scenes; "
            f"peak instances={peak_instances}, peak draws={peak_draws}."
        )


if __name__ == "__main__":
    main()
