"""Compile the recovered Pico timeline subset to immutable native C tables.

This is an offline compiler, not an ActionScript interpreter. Every included
script must parse completely; unsupported syntax is a fatal error. Integration
scripts and external navigation are excluded explicitly and recorded.
"""

import argparse
import ast
from collections import Counter
import hashlib
import json
from pathlib import Path
import re

EXCLUDED_SYMBOLS = {34, 47, 48}
STARTUP_SCRIPTS = (
    ("exports/scripts/frame_1/DoAction.as", "root", "frame", 1, None),
    ("exports/scripts/DefineSprite_22/frame_1/DoAction.as", 22, "frame", 1, None),
    ("exports/scripts/DefineSprite_22/frame_2/DoAction.as", 22, "frame", 2, None),
    ("exports/scripts/DefineButton2_15/BUTTONCONDACTION on(release).as", 15, "button", None, "release"),
    ("exports/scripts/DefineButton2_21/BUTTONCONDACTION on(release).as", 21, "button", None, "release"),
)
# Only these exact recovered scripts use offline substitutions. Any changed
# bootstrap must be reviewed before its network/loader behaviour is replaced.
OFFLINE_BOOTSTRAP = {
    "exports/scripts/frame_1/DoAction.as": (
        "d04b1216ea0829a387373cdd3bd03dc01abfe56ec56429153a0fed7a6fda4f7b",
        "stop();",
        "Keep the original stop; Flash context-menu and Newgrounds SDK settings are unused.",
    ),
    "exports/scripts/DefineSprite_22/frame_1/DoAction.as": (
        "890c4d290512d23ae58ab28f2fc14040c67494f624e0a65767adae35eb89db2f",
        'tellTarget("/"){stop();} gotoAndStop("loaded");',
        "Bundled assets are already loaded; keep the original loaded frame and its Play button.",
    ),
}
# Gameplay corrections remain separate from the offline loader substitutions.
# Pin the recovered source so later extraction changes need an explicit review.
GAMEPLAY_FIXES = {
    "exports/scripts/DefineButton2_746/BUTTONCONDACTION on(release).as": (
        "3018afacc52509a07be11dfb905302fc21156af898448a5a615bf79e56e13e47",
        'on(release){ tellTarget("/pico"){gotoAndStop(7);} '
        'tellTarget("/music"){gotoAndPlay(2);} '
        'tellTarget("/action_room9"){play();} tellTarget("/"){gotoAndStop(9);} }',
        "Restore hallway music when leaving Hanzou's room. The recovered exit omits "
        "the music-controller reset used by other exits, leaving the reward track looping.",
    ),
    "exports/scripts/DefineSprite_233/frame_14/DoAction.as": (
        "269d5f8c06698c66c40c9f8831ed4705ccaebf8d5dfcace6a441d828fbef9b9a",
        'if (_root._currentframe == 3) { tellTarget("/action_semi") { gotoAndStop(3); } }',
        "Restore cinema mode after the key pickup only while still in Alucard's room. "
        "The persistent inventory animation can finish after the exit has cleared the message.",
    ),
}
FLAGS = {"marcsummers": 0, "tapthat": 1}
OPS = {"play": 1, "stop": 2, "nextFrame": 3, "gotoAndPlay": 4, "gotoAndStop": 5, "stopAllSounds": 8}
TOKEN = re.compile(r'\s+|//[^\n]*|"(?:\\.|[^"\\])*"|\d+|[A-Za-z_$][\w$]*|[{}().;,=!/]')


def read_text_export(path):
    # JPEXS inserts this marker between text records. It isn't dialogue.
    records = (
        line
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip() != "--- RECORDSEPARATOR ---"
    )
    return " ".join(" ".join(records).split())


class Compiler:
    def __init__(self, source):
        self.source = source
        self.data = json.loads((source / "analysis/structure.json").read_text())
        self.game = json.loads((source / "analysis/game_logic.json").read_text())
        self.strings = [""]
        self.string_ids = {"": 0}
        self.actions = []
        self.sounds = []
        self.envelopes = []
        self.frames = []
        self.placements = []
        self.refs = []
        self.placement_ids = {}
        self.snapshot_ids = {}
        self.action_spans = {}
        self.sound_spans = {}
        self.envelope_spans = {}
        self.scripts = {}
        self.audit = []
        self.external = []
        self.used_sources = set()
        self.tags = {t["id"]: t for t in self.data["tags"]}
        self.characters = {c["id"]: c for c in self.data["characters"]}

    def string(self, s):
        if s not in self.string_ids:
            self.string_ids[s] = len(self.strings)
            self.strings.append(s)
        return self.string_ids[s]

    def parse_script(self, text, path):
        tokens = []
        end = 0
        for m in TOKEN.finditer(text):
            if text[end : m.start()].strip():
                raise ValueError(f"{path}: unparsed syntax {text[end:m.start()]!r}")
            end = m.end()
            v = m.group()
            if not v.isspace() and not v.startswith("//"):
                tokens.append(v)
        if text[end:].strip():
            raise ValueError(f"{path}: unparsed tail {text[end:]!r}")
        i = 0

        def take(w=None):
            nonlocal i
            if i >= len(tokens):
                raise ValueError(f"{path}: unexpected end, wanted {w}")
            t = tokens[i]
            i += 1
            if w is not None and t != w:
                raise ValueError(f"{path}: wanted {w!r}, got {t!r} at token{i}")
            return t

        def string():
            return ast.literal_eval(take())

        def timeline_op(op, target):
            take("(")
            arg = 0
            code = OPS[op]
            if tokens[i] != ")":
                v = take()
                if v.startswith('"'):
                    arg = self.string(ast.literal_eval(v))
                    code += 2
                else:
                    arg = int(v)
            take(")")
            take(";")
            return (self.string(target), arg, 0, code, 0)

        def body(target="", closing=False):
            nonlocal i
            out = []
            while i < len(tokens) and tokens[i] != "}":
                op = take()
                if op == "on":
                    take("(")
                    event = take()
                    take(")")
                    take("{")
                    if event not in ("press", "release"):
                        raise ValueError(f"{path}: unsupported buttonevent {event}")
                    out += body(target, True)
                elif op == "tellTarget":
                    take("(")
                    dest = string()
                    take(")")
                    take("{")
                    out += body(dest, True)
                elif op == "if":
                    take("(")
                    condition = take()
                    if condition == "!":
                        take("_root")
                        take(".")
                        aux = FLAGS[take()]
                        guard_target, code = self.string(target), 11
                    elif condition == "_root":
                        take(".")
                        take("_currentframe")
                        take("=")
                        take("=")
                        aux = int(take())
                        if not 1 <= aux <= 65535:
                            raise ValueError(f"{path}: root frame must be between 1 and 65535")
                        guard_target, code = 0, 12
                    else:
                        raise ValueError(f"{path}: unsupported condition {condition}")
                    take(")")
                    take("{")
                    block = body(target, True)
                    out.append((guard_target, len(block), aux, code, 0))
                    out += block
                elif op == "_root":
                    take(".")
                    member = take()
                    if member in FLAGS:
                        take("=")
                        take("true")
                        take(";")
                        out.append((0, FLAGS[member], 0, 10, 0))
                    elif member == "medal_popup":
                        take(".")
                        take("unlockMedal")
                        take("(")
                        name = string()
                        take(")")
                        take(";")
                        out.append((0, self.string(name), 0, 9, 0))
                    elif member == "gotoAndStop":
                        out.append(timeline_op(member, "/"))
                    else:
                        raise ValueError(f"{path}: unsupported root member {member}")
                elif op == "getURL":
                    take("(")
                    url = string()
                    if tokens[i] == ",":
                        take(",")
                        window = string()
                    take(")")
                    take(";")
                    self.external.append({"source": path, "url": url, "handling": "offline no-op"})
                elif op in OPS:
                    out.append(timeline_op(op, target))
                else:
                    raise ValueError(f"{path}: unsupported statement {op}")
            if closing:
                take("}")
            return out

        result = body()
        if i != len(tokens):
            raise ValueError(f"{path}: trailing tokens")
        return result

    def load_scripts(self):
        scripts = list(self.game["scripts"])
        # The read-only gameplay inventory excluded the loader and its buttons.
        # Include those original sources here without rewriting the recovery.
        indexed = {s["source"] for s in scripts}
        scripts.extend(
            dict(zip(("source", "symbol", "kind", "frame", "event"), entry))
            for entry in STARTUP_SCRIPTS
            if entry[0] not in indexed
        )
        for script in scripts:
            path = script["source"]
            source = (self.source / path).read_bytes()
            digest = hashlib.sha256(source).hexdigest()
            text = source.decode("utf-8")
            adaptation = OFFLINE_BOOTSTRAP.get(path)
            source_check = "offline bootstrap source changed"
            if adaptation is None:
                adaptation = GAMEPLAY_FIXES.get(path)
                source_check = "gameplay correction source changed"
            if adaptation:
                if digest != adaptation[0]:
                    raise ValueError(f"{path}: {source_check}")
                text = adaptation[1]
            ops = self.parse_script(text, path)
            symbol = 0 if script["symbol"] == "root" else script["symbol"]
            key = (symbol, script["kind"], script["frame"], script["event"])
            self.scripts.setdefault(key, []).append((path, ops))
            audit = {"source": path, "sha256": digest, "op_count": len(ops)}
            if adaptation:
                audit.update(native_equivalent=text, handling=adaptation[2])
            self.audit.append(audit)

    def span(self, values, table, index):
        key = tuple(values)
        if key not in index:
            index[key] = (len(table), len(values))
            table.extend(values)
        return index[key]

    def action_span(self, symbol, frame=None, event=None):
        kind = "button" if event else "frame"
        blocks = self.scripts.get((symbol, kind, frame, event), [])
        # JPEXS DoAction.as is first; DoAction_2.as follows in tag order.
        blocks = sorted(
            blocks,
            key=lambda p: (
                int(re.search(r"DoAction_(\d+)", p[0]).group(1))
                if re.search(r"DoAction_(\d+)", p[0])
                else 1
            ),
        )
        self.used_sources.update(p for p, _ in blocks)
        return self.span([o for _, ops in blocks for o in ops], self.actions, self.action_spans)

    def matrix(self, p):
        m = p.get("matrix", {})
        return (
            m.get("scale_x_16_16", 65536),
            m.get("rotate_skew_0_16_16", 0),
            m.get("rotate_skew_1_16_16", 0),
            m.get("scale_y_16_16", 65536),
            round(m.get("translate_x_twips", 0) * 65536 / 20),
            round(m.get("translate_y_twips", 0) * 65536 / 20),
        )

    def placement(self, p, life):
        color = p.get("color_transform", {})
        key = (
            self.matrix(p),
            tuple(color.get("multiply_8_8", [256] * 4)),
            tuple(color.get("add", [0] * 4)),
            life,
            p["character_id"],
            p["depth"],
            self.string(p.get("name", "")),
            p.get("clip_depth", 0),
            p.get("ratio", 0),
            0,
        )
        if key not in self.placement_ids:
            self.placement_ids[key] = len(self.placements)
            self.placements.append(key)
        return self.placement_ids[key]

    def sound(self, d):
        info = d.get("sound_info", {})
        if "in_point" in info or "out_point" in info:
            raise ValueError("Sound in/out points aren't supported by the native cue format")
        envelopes = []
        for point in info.get("envelopes", []):
            position, left, right = (
                point["position_44khz"], point["left_level"], point["right_level"]
            )
            if not 0 <= position <= 0xFFFFFFFF or not 0 <= left <= 32768 or not 0 <= right <= 32768:
                raise ValueError("Sound envelope position or level is outside the SWF range")
            if envelopes and position < envelopes[-1][0]:
                raise ValueError("Sound envelope points must be in sample order")
            envelopes.append((position, left, right))
        if len(envelopes) > 255:
            raise ValueError("Too many sound envelope points for SWF SOUNDINFO")
        envelope_span = self.span(envelopes, self.envelopes, self.envelope_spans)
        return (
            d["sound_id"],
            info.get("loop_count", 1),
            int(info.get("sync_stop", False)),
            int(info.get("sync_no_multiple", False)),
            envelope_span[1],
            envelope_span[0],
        )

    def build(self):
        self.load_scripts()
        self.symbols = [(0, 0, 0, 0, 0, 0, 0, 0) for _ in range(1025)]
        # Leaf data comes from the independently generated graphics pack.
        for cid, char in self.characters.items():
            if cid >= 1025:
                continue
            if char["kind"] not in ("DefineSprite", "DefineButton2", "DefineSound", "DefineFont3"):
                self.symbols[cid] = (0, 0, 0, 0, 0, 0, 1, 0)
        life_counter = 1
        for timeline in self.data["timelines"]:
            symbol = 0 if timeline["id"] == "root" else int(timeline["id"].split("_")[1])
            if symbol >= 1025 or symbol in EXCLUDED_SYMBOLS:
                continue
            first_frame = len(self.frames)
            lives = {}
            for frame in timeline["frames"]:
                sounds = []
                for tag_id in frame["tag_ids"]:
                    tag = self.tags[tag_id]
                    d = tag.get("data", {})
                    if tag["code"] == 26:
                        depth = d["depth"]
                        if "character_id" in d or depth not in lives:
                            lives[depth] = life_counter
                            life_counter += 1
                    elif tag["code"] == 28:
                        lives.pop(d["depth"], None)
                    elif tag["code"] == 15:
                        sounds.append(self.sound(d))
                ps = []
                for p in frame["display_list"]:
                    if p["character_id"] in EXCLUDED_SYMBOLS or p["character_id"] >= 1025:
                        continue
                    ps.append(self.placement(p, lives[p["depth"]]))
                placement_span = self.span(ps, self.refs, self.snapshot_ids)
                action_span = self.action_span(symbol, frame["frame"])
                sound_span = self.span(sounds, self.sounds, self.sound_spans)
                label = self.string(frame["labels"][0]) if frame["labels"] else 0
                self.frames.append(
                    (
                        placement_span[0],
                        action_span[0],
                        sound_span[0],
                        placement_span[1],
                        action_span[1],
                        sound_span[1],
                        label,
                    )
                )
            self.symbols[symbol] = (first_frame, 0, 0, len(timeline["frames"]), 0, 0, 2, 0)
        button_sounds = {
            t["data"]["button_id"]: t["data"]["events"]
            for t in self.data["tags"]
            if t["code"] == 17
        }
        for events in button_sounds.values():
            if any(e["sound_id"] and e["event"] != "over_up_to_over_down" for e in events):
                raise ValueError("Native button cues currently support authored press sounds only")
        for tag in self.data["tags"]:
            if tag["code"] != 34:
                continue
            d = tag["data"]
            symbol = d["character_id"]
            if symbol in EXCLUDED_SYMBOLS or (symbol < 55 and symbol not in (15, 21)):
                continue
            first = len(self.frames)
            record_lives = list(range(life_counter, life_counter + len(d["records"])))
            life_counter += len(d["records"])
            for state in ("up", "over", "down", "hit_test"):
                ps = []
                for ri, record in enumerate(d["records"]):
                    if state not in record["states"]:
                        continue
                    ps.append(self.placement(record, record_lives[ri]))
                span = self.span(ps, self.refs, self.snapshot_ids)
                # Keep press cues beside the down artwork. Runtime dispatches
                # them on a press, not whenever a drag restores that artwork.
                event = "over_up_to_over_down" if state == "down" else None
                ss = [
                    self.sound(s)
                    for s in button_sounds.get(symbol, [])
                    if s["sound_id"] and s["event"] == event
                ]
                snd = self.span(ss, self.sounds, self.sound_spans)
                self.frames.append((span[0], 0, snd[0], span[1], 0, snd[1], 0))
            press = self.action_span(symbol, event="press")
            release = self.action_span(symbol, event="release")
            self.symbols[symbol] = (first, press[0], release[0], 4, press[1], release[1], 3, 0)
        unused = {a["source"] for a in self.audit} - self.used_sources
        if unused:
            raise ValueError(
                "Compiled scripts were not linked to timeline/button: " + str(sorted(unused))
            )
        if len(self.placements) > 65535:
            raise ValueError("Too many placements for uint16 references")
        return self

    def write(self, out):
        # Check before creating or replacing output. PicoFrame stores these
        # starts in 16 bits; placement references retain their 32-bit range.
        for number, frame in enumerate(self.frames, 1):
            for field, value in zip(("first_action", "first_sound"), frame[1:3]):
                if not 0 <= value <= 65535:
                    raise ValueError(
                        f"Frame {number} {field} offset {value} is outside uint16 range"
                    )
        out.mkdir(parents=True, exist_ok=True)
        colors = []
        color_ids = {}
        for p in self.placements:
            key = (p[1], p[2])
            if key not in color_ids:
                color_ids[key] = len(colors)
                colors.append(key)

        def nums(v):
            return "{" + ",".join(map(str, v)) + "}"

        lines = [
            "/* Generated from the supplied SWF. Do not edit. */",
            '#include "game_data.h"',
            "",
        ]
        for typ, name, rows in [
            ("PicoSoundEnvelope", "sound_envelopes", self.envelopes),
            ("PicoSymbol", "symbols", self.symbols),
            ("PicoFrame", "frames", self.frames),
            ("PicoAction", "actions", self.actions),
        ]:
            lines.append(
                f"static const {typ} {name}[]={{\n" + ",\n".join(nums(v) for v in rows) + "\n};"
            )
        lines.append("static const PicoSoundEvent sounds[]={")
        for *fields, first in self.sounds:
            pointer = f"sound_envelopes+{first}" if fields[-1] else "0"
            lines.append("{" + ",".join(map(str, fields)) + "," + pointer + "},")
        lines.append("};")
        lines.append(
            "static const uint16_t placement_refs[]={\n"
            + ",\n".join(
                ",".join(map(str, self.refs[i : i + 24])) for i in range(0, len(self.refs), 24)
            )
            + "\n};"
        )
        lines.append("static const PicoPlacement placements[]={")
        for m, mul, add, *rest in self.placements:
            lines.append(
                "{" + nums(m) + "," + ",".join(map(str, rest[:-1] + [color_ids[(mul, add)]])) + "},"
            )
        lines += [
            "};",
            "static const PicoColor colors[]={"
            + ",".join("{" + nums(m) + "," + nums(a) + "}" for m, a in colors)
            + "};",
            "static const char *const strings[]={"
            + ",".join(json.dumps(s) for s in self.strings)
            + "};",
        ]
        lines.append(
            "const PicoData pico_game_data={symbols,frames,placement_refs,placements,colors,actions,sounds,strings,"
            + ",".join(
                map(
                    str,
                    [
                        len(self.frames),
                        len(self.refs),
                        len(self.placements),
                        len(self.actions),
                        len(self.sounds),
                        len(self.symbols),
                        len(self.strings),
                        len(colors),
                    ],
                )
            )
            + "};"
        )
        labels = self.button_labels()
        lines += ["const char *pico_button_label(uint16_t symbol){switch(symbol){"]
        lines += [f"case {k}:return {json.dumps(v)};" for k, v in sorted(labels.items())]
        lines += [
            'default:return "INTERACT";}}',
            "const char *pico_text_for_symbol(uint16_t symbol){switch(symbol){",
        ]
        for path in sorted((self.source / "exports/texts").glob("*.txt")):
            text = read_text_export(path)
            if text:
                lines.append(f"case {int(path.stem)}:return {json.dumps(text,ensure_ascii=True)};")
        lines += ['default:return "";}}', ""]
        (out / "game_data.c").write_text("\n".join(lines), encoding="utf-8")
        (out / "game_data.h").write_text(
            '#ifndef PICO_GAME_DATA_H\n#define PICO_GAME_DATA_H\n#include "pico.h"\nextern const PicoData pico_game_data;\nconst char *pico_button_label(uint16_t symbol);\nconst char *pico_text_for_symbol(uint16_t symbol);\n#endif\n',
            encoding="utf-8",
        )
        report = {
            "source_sha256": self.data["header"]["source_sha256"],
            "compiled_scripts": len(self.audit),
            "opcode_count": len(self.actions),
            "symbols": len(self.symbols),
            "frames": len(self.frames),
            "placements": len(self.placements),
            "placement_references": len(self.refs),
            "strings": len(self.strings),
            "sound_cues": len(self.sounds),
            "sound_envelope_points": len(self.envelopes),
            "snapshot_original_references": sum(
                len(f["display_list"]) for t in self.data["timelines"] for f in t["frames"]
            ),
            "source_scripts": self.audit,
            "external_navigation": self.external,
            "integration_symbols_removed": sorted(EXCLUDED_SYMBOLS),
            "offline_bootstrap": [a for a in self.audit if a["source"] in OFFLINE_BOOTSTRAP],
            "gameplay_fixes": [a for a in self.audit if a["source"] in GAMEPLAY_FIXES],
            "button_labels": labels,
            "known_limits": [
                "Frame scheduling must be compared to Flash runtime.",
                "StartSound loops, no-multiple flags and envelopes are retained. All 23 authored button sound records contain press cues only. In/out points and other button sound transitions fail compilation if introduced.",
                "Gameplay guards support two flag bits and exact root-frame comparisons; Newgrounds API replaced by local medal callback.",
                "UI labels are editorial accessibility hints; original dialogue is separately retained in text catalog.",
            ],
        }
        (out / "compile_report.json").write_text(
            json.dumps(report, indent=2) + "\n", encoding="utf-8"
        )
        print(
            json.dumps(
                {k: v for k, v in report.items() if k not in ("source_scripts", "button_labels")},
                indent=2,
            )
        )

    def button_labels(self):
        labels = {
            15: "WEBSITE",
            21: "PLAY",
            84: "GO HALL",
            88: "OPEN DOOR",
            120: "CONTINUE",
            207: "TAKE GUN",
            219: "USE GUN",
            224: "USE EXTINGUISHER",
            228: "USE GOGGLES",
            232: "USE SCHOOL KEY",
            235: "USE TEACHER KEY",
            240: "USE HERB",
            288: "WEBSITE",
            341: "SHOOT ALUCARD",
            371: "GO HALL",
            378: "SKIP DIALOGUE",
            382: "TAKE SCHOOL KEY",
            418: "GO CLASSROOM",
            419: "GO EAST",
            432: "GO EAST",
            433: "GO WEST",
            434: "OPEN CLASSROOM",
            437: "TAKE EXTINGUISHER",
            438: "GO NORTH",
            449: "GO EAST",
            450: "GO WEST",
            451: "OPEN CLOSET",
            463: "GO SOUTH",
            511: "LEAVE",
            557: "GO EAST",
            558: "GO WEST",
            579: "TAKE HERB",
            591: "GO WEST",
            593: "GO EAST",
            594: "ENTER RESTROOM",
            595: "ENTER RESTROOM",
            604: "GO EAST",
            605: "GO WEST",
            647: "GO EAST",
            648: "OPEN LOUNGE",
            656: "GO WEST",
            659: "OPEN FINAL DOOR",
            703: "GO SOUTH",
            746: "GO SOUTH",
            751: "TAKE GOGGLES",
            756: "GO CLASSROOM",
            757: "GO SOUTH",
            763: "GO SOUTH",
            766: "TAKE TEACHER KEY",
            885: "SHOOT WEAK SPOT",
            911: "SHOOT OBJECT",
            917: "SHOOT",
        }
        for (symbol, kind, frame, event), blocks in self.scripts.items():
            if kind != "button" or symbol in labels:
                continue
            text = " ".join((self.source / path).read_text() for path, _ in blocks)
            if "nextFrame" in text or "gotoAndStop(22)" in text:
                labels[symbol] = "SHOOT"
            elif "gotoAndStop(23)" in text:
                labels[symbol] = "SHOOT"
            elif "gotoAndStop(2)" in text and 'tellTarget("/")' in text:
                labels[symbol] = "RESTART"
            elif "/continue" in text:
                labels[symbol] = "CONTINUE"
            else:
                labels[symbol] = "INTERACT"
        return labels


if __name__ == "__main__":
    base = Path(__file__).resolve().parents[1]
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", type=Path, default=base.parent / "pico-decomp")
    ap.add_argument("--output", type=Path, default=base / "generated")
    a = ap.parse_args()
    Compiler(a.source.resolve()).build().write(a.output.resolve())
