# pico-c

Pico's School rebuilt in portable C99, intended as a base for ports to other devices, including low-powered hardware and very small screens.

The codebase includes the game logic, renderer, audio mixer, generated game tables and artwork/sound packs. It preserves the original menu, intro, dialogue and gameplay. Game time advances at 24 Hz independently of drawing speed. The core uses fixed memory pools with no heap allocation, and audio, framebuffers and artwork caches are optional.

- `src/` and `include/`: portable runtime, renderer and mixer.
- `generated/` and `assets/`: game tables, artwork and sound banks.
- `platform/`: reference C adapters for display, input and audio.
- `tests/` and `tools/`: regression checks, build scripts and content tools.

A new port supplies its own platform adapter around the shared C code. The desktop and headless hosts are reference implementations for development and testing. See [porting notes](docs/PORTING.md) for the interfaces, memory budgets and fixes to preserve. Embedded hardware support still needs implementation and testing on each target.

Pico's School was created by Tom Fulp / Newgrounds. This is an unofficial project; the original game, artwork and audio belong to their respective creators.
