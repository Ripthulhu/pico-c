"""Mask rejection doesn't require the separate recovered source tree."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from compile_game import Compiler

compiler = Compiler.__new__(Compiler)
compiler.placement_ids, compiler.placements = {}, []
compiler.strings, compiler.string_ids = [""], {"": 0}
placement = {"character_id": 7, "depth": 1}
assert compiler.placement(placement, 1) == compiler.placement({**placement, "clip_depth": 0}, 1)
for depth in (-1, 1, 65535):
    try:
        compiler.placement({**placement, "clip_depth": depth}, 1)
    except ValueError as error:
        assert "Clip masks" in str(error)
    else:
        raise AssertionError("Masked placement was accepted")
assert len(compiler.placements) == 1
