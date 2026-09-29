# Porting notes

This snapshot contains the shared C code from development revision `6dad68f8280fe3024848fb1ad54e706d27dd1c4b`, including the latest table, scanline and mixer optimisations. Platform-specific app code and development logs aren't included.

## Adapter contract

Initialise `PicoAssets`, set the `PicoHost` callbacks, then call `pico_init` with start frame 0. This selects the original menu and intro. Explicit root frames are for diagnostics. Keep the tables and selected asset banks alive for the entire game.

Call `pico_tick` at 24 Hz. Drawing can run less often, but mustn't discard simulation ticks. Bound input batches so repeated presses can't starve drawing or game time. Pause audio positions with the game. Serialise input, ticks and rendering, and hold the scene fixed across all rows of a streamed picture.

Pointer coordinates use the original 550 x 350 stage in signed 16.16 units. Map through the actual displayed picture. `platform/touch_input.c` provides optional finger tolerance, including moving Hanzou targets. Send both press and release, cancel gestures that leave the picture, and don't require hover before aiming and firing.

Small devices can render RGB565 rows or monochrome output without a framebuffer or draw list. Larger devices can keep a framebuffer and repair changed regions. Preserve overlapping artwork when repairing those regions. Don't blank the visible picture before its replacement is ready. Artwork scale, render size and physical display size are separate choices.

## Behaviour to preserve

- Original startup and dialogue stops: `tests/test_startup.c`.
- Absolute script actions still finish after their owner is removed, so combat animations continue: `tests/test_runtime.c`, `tests/test_alucard.c`, `tests/test_combat.c`.
- Touch aims and fires directly, with allowance for moving targets: `tests/test_touch.c`, `tests/test_touch_game.c`, `tests/test_hanzou_touch.c`.
- Alucard's delayed key animation can't restore the cinema message over the hallway inventory: `tests/test_cinema.c`.
- Hanzou's reward music stops when leaving his room, while authored ambience layers remain: `tests/test_music.c`.
- Tiny captions, reader pages, original graphics and cached/uncached rendering stay consistent: `tests/test_caption.c`, `tests/test_render_equivalence.c`, `tests/test_host_damage.c`.

Masks are rejected by the content compiler and runtime; the game uses none. The compiler retains the source-checked content corrections. Rebuild tables and runtime together: frame records use 16-bit action/sound starts and 32-bit placement starts. The checked-in tables are ready to compile. Regenerating them or the asset banks requires the separate recovery data expected by the Python tools. Source-comparison tests skip when that data isn't present.

## Small-device budget

Keep persistent game state below 16 KiB and aim for at most 64 KiB of game-owned RAM, including stacks and display/audio buffers. Defaults use 320 instances and 128 queued actions. Target-specific builds can override the pool sizes in the headers; size them against the game's conservative instance bound.

The S3 compiler measures 11,088 bytes for `Pico`, 28 for `PicoAssets`, and 464 for the optional eight-voice mixer. Core code/tables total 557,449 bytes. With the mixer, micro artwork and micro sound bank, the total is 1,432,889 bytes before platform code, libraries and linker overhead. The audio render function reports 112 bytes of stack; this isn't a whole call-chain limit.

Large colour packs, decoded caches, retained draw lists and full framebuffers are optional costs. The indexed scanline path needs none of them. Tables currently require directly addressable storage. Banked ROM needs an adapter. These are compiler measurements and design budgets, not a working embedded firmware claim.

Run both build profiles after shared changes. Pixel and audio equivalence checks protect the optimisations. Measure complete frame time, display transfers and stack high-water marks on each target before claiming support.
