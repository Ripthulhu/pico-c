import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from compile_game import Compiler, GAMEPLAY_FIXES, OFFLINE_BOOTSTRAP, STARTUP_SCRIPTS, read_text_export


class CompilerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.c = Compiler(ROOT.parent / "pico-decomp").build()

    def test_every_game_source_parses_and_links(self):
        self.assertEqual(len(self.c.audit), 747)
        self.assertEqual(len(self.c.used_sources), 747)
        self.assertTrue(all(len(x["sha256"]) == 64 for x in self.c.audit))

    def test_dialogue_omits_export_markers(self):
        folder = ROOT.parent / "pico-decomp/exports/texts"
        self.assertEqual(
            read_text_export(folder / "315.txt"), "You sure are a nosey bastard, Pico."
        )
        for path in folder.glob("*.txt"):
            self.assertNotIn("RECORDSEPARATOR", read_text_export(path), path.name)

    def test_target_scope_and_flag_guard(self):
        c = self.c
        ops = c.parse_script('tellTarget("/health"){play();} stop();', "fixture")
        self.assertEqual(c.strings[ops[0][0]], "/health")
        self.assertEqual(ops[0][3], 1)
        self.assertEqual(ops[1][0], 0)
        self.assertEqual(ops[1][3], 2)
        guard = c.parse_script(
            'if(!_root.tapthat){_root.tapthat=true;_root.medal_popup.unlockMedal("Tap That");} stop();',
            "guard",
        )
        self.assertEqual(guard[0][1:4], (2, 1, 11))
        self.assertEqual(guard[1][1:4], (1, 0, 10))
        self.assertEqual(guard[-1][3], 2)

    def test_unknown_code_fails_closed(self):
        for text in (
            'trace("not supported");',
            "play(); @ stop();",
            "if(foo){stop();}",
            "gotoAndStop(2); extra",
            '_root.getURL("somewhere");',
        ):
            with self.assertRaises((ValueError, KeyError)):
                self.c.parse_script(text, "must-reject")

    def test_root_frame_guard_keeps_scope_and_nested_flag_skip(self):
        c = self.c
        ops = c.parse_script(
            'tellTarget("/health"){if(_root._currentframe == 3){'
            'if(!_root.tapthat){play();} nextFrame();} stop();}',
            "root-frame-guard",
        )
        self.assertEqual(ops[0], (0, 3, 3, 12, 0))
        self.assertEqual(ops[1][1:4], (1, 1, 11))
        self.assertEqual([op[3] for op in ops[2:]], [1, 3, 2])
        for op in ops[1:]:
            self.assertEqual(c.strings[op[0]], "/health")
        for frame in (1, 65535):
            ops = c.parse_script(f"if(_root._currentframe == {frame}){{stop();}}", "root-frame-edge")
            self.assertEqual(ops[0], (0, 1, frame, 12, 0))

    def test_root_frame_guard_rejects_invalid_or_unimplemented_conditions(self):
        for condition in (
            "_root._currentframe == 0",
            "_root._currentframe == 65536",
            "_root._currentframe == -1",
            "_root._currentframe == 3.5",
            '_root._currentframe == "3"',
            "_root._currentframe = 3",
            "_root._currentframe != 3",
            "_root.currentframe == 3",
            "_root._currentframe == frame",
            "!_root._currentframe",
        ):
            with self.subTest(condition=condition):
                with self.assertRaises((ValueError, KeyError)):
                    self.c.parse_script(f"if({condition}){{stop();}}", "must-reject-frame-guard")

    def placements(self, symbol, frame):
        s = self.c.symbols[symbol]
        f = self.c.frames[s[0] + frame - 1]
        return [self.c.placements[i] for i in self.c.refs[f[0] : f[0] + f[3]]]

    def sounds(self, symbol, frame):
        f = self.c.frames[self.c.symbols[symbol][0] + frame - 1]
        return self.c.sounds[f[2] : f[2] + f[5]]

    def actions(self, symbol, frame):
        f = self.c.frames[self.c.symbols[symbol][0] + frame - 1]
        return self.c.actions[f[1] : f[1] + f[4]]

    def test_loaded_menu_reaches_original_intro(self):
        c = self.c
        self.assertEqual([p[4] for p in self.placements(0, 1)], [22])
        self.assertEqual(self.actions(0, 1), [(0, 0, 0, 2, 0)])
        loading = self.actions(22, 1)
        self.assertEqual((c.strings[loading[0][0]], loading[0][3]), ("/", 2))
        self.assertEqual((c.strings[loading[1][1]], loading[1][3]), ("loaded", 7))
        loaded = c.frames[c.symbols[22][0] + 2]
        self.assertEqual(c.strings[loaded[6]], "loaded")
        self.assertEqual([p[4] for p in self.placements(22, 3)], [21, 15])
        for button in (15, 21):
            self.assertEqual(c.symbols[button][6], 3)
            for state in range(1, 5):
                self.assertTrue(self.placements(button, state))
        play = c.symbols[21]
        self.assertEqual(play[5], 1)
        target, label, _, op, _ = c.actions[play[2]]
        self.assertEqual((c.strings[target], c.strings[label], op), ("/", "intro", 7))
        intro = c.frames[c.symbols[0][0] + 22]
        self.assertEqual(c.strings[intro[6]], "intro")
        self.assertEqual([p[4] for p in self.placements(0, 23)], [1024])
        final = self.actions(1024, 171)[-1]
        self.assertEqual((c.strings[final[0]], final[1], final[3]), ("/", 2, 5))
        self.assertEqual(c.symbols[15][5], 0)
        self.assertTrue(any(e["source"] == STARTUP_SCRIPTS[3][0] for e in c.external))

    def test_offline_bootstrap_is_source_checked_and_audited(self):
        audited = {a["source"]: a for a in self.c.audit}
        for entry in STARTUP_SCRIPTS:
            self.assertIn(entry[0], audited)
            self.assertIn(entry[0], self.c.used_sources)
        for path, (digest, native, reason) in OFFLINE_BOOTSTRAP.items():
            self.assertEqual(audited[path]["sha256"], digest)
            self.assertEqual(audited[path]["native_equivalent"], native)
            self.assertEqual(audited[path]["handling"], reason)
        path = STARTUP_SCRIPTS[0][0]
        _, native, reason = OFFLINE_BOOTSTRAP[path]
        with patch.dict(OFFLINE_BOOTSTRAP, {path: ("0" * 64, native, reason)}):
            with self.assertRaisesRegex(ValueError, "offline bootstrap source changed"):
                Compiler(ROOT.parent / "pico-decomp").load_scripts()

    def test_key_pickup_cinema_guard_is_source_checked_and_audited(self):
        path = "exports/scripts/DefineSprite_233/frame_14/DoAction.as"
        digest, native, reason = GAMEPLAY_FIXES[path]
        audited = {a["source"]: a for a in self.c.audit}
        self.assertEqual(audited[path]["sha256"], digest)
        self.assertEqual(audited[path]["native_equivalent"], native)
        self.assertEqual(audited[path]["handling"], reason)
        ops = self.actions(233, 14)
        self.assertEqual(ops[0], (0, 1, 3, 12, 0))
        self.assertEqual((self.c.strings[ops[1][0]], ops[1][1:]), ("/action_semi", (3, 0, 5, 0)))
        # The separate authored stop remains outside the conditional block.
        self.assertEqual(ops[2:], [(0, 0, 0, 2, 0)])
        stop_path = "exports/scripts/DefineSprite_233/frame_14/DoAction_2.as"
        self.assertNotIn("native_equivalent", audited[stop_path])
        with patch.dict(GAMEPLAY_FIXES, {path: ("0" * 64, native, reason)}):
            with self.assertRaisesRegex(ValueError, "gameplay correction source changed"):
                Compiler(ROOT.parent / "pico-decomp").load_scripts()

    def test_report_separates_gameplay_corrections_from_offline_bootstrap(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            with contextlib.redirect_stdout(io.StringIO()):
                self.c.write(output)
            report = json.loads((output / "compile_report.json").read_text())
        self.assertEqual({a["source"] for a in report["offline_bootstrap"]}, set(OFFLINE_BOOTSTRAP))
        self.assertEqual({a["source"] for a in report["gameplay_fixes"]}, set(GAMEPLAY_FIXES))
        self.assertEqual(report["compiled_scripts"], 747)

    def test_frame_action_and_sound_offsets_reject_overflow_before_writing(self):
        for field, index in (("first_action", 1), ("first_sound", 2)):
            for value in (-1, 65536):
                with self.subTest(field=field, value=value):
                    frame = list(self.c.frames[0])
                    frame[index] = value
                    with tempfile.TemporaryDirectory() as folder:
                        output = Path(folder) / "generated"
                        with patch.object(self.c, "frames", [tuple(frame)]):
                            with self.assertRaisesRegex(ValueError, field + " offset"):
                                self.c.write(output)
                        self.assertFalse(output.exists())

    def test_frame_offsets_keep_unsigned_boundaries_and_placement_range(self):
        frames = [(0, 0, 0, 0, 0, 0, 0), (4294967295, 65535, 65535, 0, 0, 0, 0)]
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            with patch.object(self.c, "frames", frames):
                with contextlib.redirect_stdout(io.StringIO()):
                    self.c.write(output)
            generated = (output / "game_data.c").read_text()
        self.assertIn(
            "static const PicoFrame frames[]={\n"
            "{0,0,0,0,0,0,0},\n"
            "{4294967295,65535,65535,0,0,0,0}\n};",
            generated,
        )

    def test_hanzou_exit_music_reset_is_source_checked_and_audited(self):
        path = "exports/scripts/DefineButton2_746/BUTTONCONDACTION on(release).as"
        digest, native, reason = GAMEPLAY_FIXES[path]
        audited = {a["source"]: a for a in self.c.audit}
        self.assertEqual(audited[path]["sha256"], digest)
        self.assertEqual(audited[path]["native_equivalent"], native)
        self.assertEqual(audited[path]["handling"], reason)
        button = self.c.symbols[746]
        ops = self.c.actions[button[2] : button[2] + button[5]]
        self.assertEqual(
            [(self.c.strings[target], arg, op) for target, arg, _, op, _ in ops],
            [("/pico", 7, 5), ("/music", 2, 4), ("/action_room9", 0, 1), ("/", 9, 5)],
        )
        with patch.dict(GAMEPLAY_FIXES, {path: ("0" * 64, native, reason)}):
            with self.assertRaisesRegex(ValueError, "gameplay correction source changed"):
                Compiler(ROOT.parent / "pico-decomp").load_scripts()

    def test_authored_sound_envelopes_survive(self):
        self.assertEqual(len(self.c.envelopes), 21)
        quiet = self.sounds(59, 19)[0]
        self.assertEqual(quiet[:5], (52, 1, 0, 0, 1))
        self.assertEqual(self.c.envelopes[quiet[5]], (0, 6394, 8791))
        fade = self.sounds(930, 285)[0]
        self.assertEqual(fade[:5], (321, 1, 0, 0, 2))
        self.assertEqual(
            self.c.envelopes[fade[5] : fade[5] + fade[4]],
            [(46656, 32768, 32768), (62208, 0, 0)],
        )
        self.assertEqual(self.sounds(243, 3)[0][:4], (215, 200, 0, 1))
        self.assertEqual(self.sounds(1024, 2)[0][:5], (992, 1000, 0, 0, 2))

    def test_every_authored_button_sound_is_linked(self):
        count = 0
        for tag in self.c.data["tags"]:
            if tag["code"] != 17:
                continue
            symbol = tag["data"]["button_id"]
            authored = [e for e in tag["data"]["events"] if e["sound_id"]]
            self.assertEqual(len(authored), 1)
            self.assertEqual(authored[0]["event"], "over_up_to_over_down")
            self.assertEqual(self.sounds(symbol, 3)[0][0], authored[0]["sound_id"])
            for frame in (1, 2, 4):
                self.assertEqual(self.sounds(symbol, frame), [])
            count += 1
        self.assertEqual(count, 23)
        self.assertEqual(self.sounds(219, 3)[0][1], 4)

    def test_unimplemented_sound_semantics_fail_compilation(self):
        for key in ("in_point", "out_point"):
            with self.assertRaisesRegex(ValueError, "in/out"):
                self.c.sound({"sound_id": 52, "sound_info": {key: 0}})
        c = Compiler(ROOT.parent / "pico-decomp")
        tag = next(t for t in c.data["tags"] if t["code"] == 17)
        tag["data"]["events"][0]["sound_id"] = 52
        with self.assertRaisesRegex(ValueError, "press sounds only"):
            c.build()

    def test_invalid_sound_envelopes_fail_compilation(self):
        for points in (
            [{"position_44khz": 0, "left_level": 32769, "right_level": 0}],
            [
                {"position_44khz": 2, "left_level": 0, "right_level": 0},
                {"position_44khz": 1, "left_level": 0, "right_level": 0},
            ],
        ):
            with self.assertRaises(ValueError):
                self.c.sound({"sound_id": 52, "sound_info": {"envelopes": points}})

    def test_persistent_controllers_keep_lifetime(self):
        def named(frame):
            return {self.c.strings[p[6]]: p[3] for p in self.placements(0, frame) if p[6]}

        start, final = named(2), named(18)
        for name in ("health", "action_gun", "continue", "icon_herb"):
            self.assertEqual(start[name], final[name], name)

    def test_replay_recreates_controllers_and_shared_button_records_survive_hover(self):
        # Root 1 removes every gameplay placement before Play enters the intro.
        root1 = self.placements(0, 1)
        root2 = self.placements(0, 2)
        self.assertEqual([p[4] for p in root1], [22])
        self.assertFalse({p[3] for p in root1} & {p[3] for p in root2})
        self.assertFalse(any(p[6] for p in root1))
        self.assertEqual(self.actions(0, 1), [(0, 0, 0, 2, 0)])
        over = {p[3] for p in self.placements(84, 2)}
        down = {p[3] for p in self.placements(84, 3)}
        self.assertEqual(over, down)
        self.assertTrue(over)


if __name__ == "__main__":
    unittest.main()
