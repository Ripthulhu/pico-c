#ifndef PICO_H
#define PICO_H

/* Freestanding C99 timeline runtime. All authored data stays in read-only ROM.
 * Coordinates and affine matrix coefficients are signed 16.16 pixels.
 * Advance exactly 24 ticks per second; render and pointer updates are independent.
 */
#include <stddef.h>
#include <stdint.h>

#ifndef PICO_MAX_INSTANCES
#define PICO_MAX_INSTANCES 320
#endif
#ifndef PICO_MAX_QUEUE
#define PICO_MAX_QUEUE 128
#endif
#ifndef PICO_MAX_NESTING
#define PICO_MAX_NESTING 32
#endif
#ifndef PICO_MAX_ACTIONS_PER_EVENT
#define PICO_MAX_ACTIONS_PER_EVENT 8192
#endif

#define PICO_NONE UINT16_MAX
#define PICO_IDENTITY {65536, 0, 0, 65536, 0, 0}
#define PICO_COLOR_IDENTITY {{256, 256, 256, 256}, {0, 0, 0, 0}}

typedef struct PicoMatrix {
    int32_t a, b, c, d, tx, ty;
} PicoMatrix;
/* RGBA multiply /256, followed by addition. */
typedef struct PicoColor {
    int16_t mul[4], add[4];
} PicoColor;

enum PicoSymbolKind { PICO_EMPTY = 0, PICO_LEAF = 1, PICO_MOVIE = 2, PICO_BUTTON = 3 };
enum PicoOp {
    PICO_NOP = 0,
    PICO_PLAY = 1,
    PICO_STOP = 2,
    PICO_NEXT = 3,
    PICO_GOTO_PLAY = 4,
    PICO_GOTO_STOP = 5,
    PICO_LABEL_PLAY = 6,
    PICO_LABEL_STOP = 7,
    PICO_STOP_SOUNDS = 8,
    PICO_MEDAL = 9,
    PICO_SET_FLAG = 10,
    /* Skip arg following operations if flag bit aux is already set. */
    PICO_IF_FLAG_SKIP = 11,
    /* Skip arg following operations unless the root is at frame aux. */
    PICO_IF_ROOT_NOT_FRAME_SKIP = 12
};

/* target: string table index; empty string = self. arg = frame, label-string
 * index, medal-string index, flag bit, or skip count, according to opcode.
 * Button actions execute in the button's parent movie context.
 */
typedef struct PicoAction {
    uint16_t target, arg, aux;
    uint8_t op, reserved;
} PicoAction;
/* Envelope positions use 44,100 samples per second regardless of source rate.
 * Channel levels use 32768 for full volume. All points remain in read-only ROM. */
typedef struct PicoSoundEnvelope {
    uint32_t position;
    uint16_t left, right;
} PicoSoundEnvelope;
typedef struct PicoSoundEvent {
    uint16_t symbol, loops;
    uint8_t stop, no_multiple;
    uint16_t envelope_count;
    const PicoSoundEnvelope *envelope;
} PicoSoundEvent;

/* A stable life ID identifies one placement lifetime across authored frames.
 * A move changes this immutable record's transform but retains its life ID.
 * name is a string index; 0 must refer to an empty string. clip_depth is 0
 * for ordinary content; nonzero clip masks are surfaced by the draw callback.
 */
typedef struct PicoPlacement {
    PicoMatrix matrix;
    uint32_t life;
    uint16_t symbol, depth, name, clip_depth, ratio, color;
} PicoPlacement;

typedef struct PicoFrame {
    /* Placement references must be ordered by ascending authored depth. */
    uint32_t first_placement;
    /* The compiler rejects action or sound starts outside this ROM range. */
    uint16_t first_action, first_sound;
    uint16_t placement_count, action_count, sound_count, label;
} PicoFrame;

/* Movie frame 1 lives at first_frame. Buttons have four frame snapshots:
 * up, over, down, hit. Leaf geometry lives outside this portable runtime.
 */
typedef struct PicoSymbol {
    uint32_t first_frame, first_press, first_release;
    uint16_t frame_count, press_count, release_count;
    uint8_t kind, reserved;
} PicoSymbol;

typedef struct PicoData {
    const PicoSymbol *symbols;
    const PicoFrame *frames;
    const uint16_t *placement_refs;
    const PicoPlacement *placements;
    const PicoColor *colors;
    const PicoAction *actions;
    const PicoSoundEvent *sounds;
    const char *const *strings;
    uint32_t frame_count, placement_ref_count, placement_count, action_count, sound_count;
    uint16_t symbol_count, string_count, color_count;
} PicoData;

enum PicoError {
    PICO_ERROR_NONE = 0,
    PICO_ERROR_INSTANCE_LIMIT = 1,
    PICO_ERROR_QUEUE_LIMIT = 2,
    PICO_ERROR_ACTION_LIMIT = 4,
    PICO_ERROR_NESTING = 8,
    PICO_ERROR_BAD_DATA = 16
};
enum PicoDrawKind { PICO_DRAW_LEAF = 0, PICO_MASK_BEGIN = 1, PICO_MASK_END = 2 };

typedef struct PicoHost {
    void *user;
    void (*draw)(void *user, uint16_t symbol, const PicoMatrix *world, const PicoColor *color,
                 uint16_t ratio, uint8_t kind);
    int (*hit)(void *user, uint16_t symbol, const PicoMatrix *world, int32_t x, int32_t y);
    void (*sound)(void *user, const PicoSoundEvent *event);
    void (*stop_sounds)(void *user);
    void (*medal)(void *user, const char *name);
} PicoHost;

/* Public for static allocation and diagnostics. Do not mutate fields directly.
 * Only movies and buttons consume instance slots; immutable leaves stay in ROM. */
typedef struct PicoInstance {
    uint32_t placement, generation, born_tick;
    uint16_t symbol, parent, child, next, frame, revision;
    uint8_t alive, playing;
} PicoInstance;
typedef struct PicoQueued {
    uint32_t first, generation;
    uint16_t count, owner, revision, reserved;
} PicoQueued;
typedef struct Pico {
    const PicoData *data;
    PicoHost host;
    PicoInstance instances[PICO_MAX_INSTANCES];
    PicoQueued queue[PICO_MAX_QUEUE];
    uint32_t tick, flags, errors, actions_executed, missing_targets, missing_labels;
    uint32_t next_generation, pressed_generation, hover_generation;
    uint16_t live_instances, peak_instances, queue_head, queue_count, peak_queue;
    uint16_t pressed, hover;
    uint8_t pointer_down, draining;
} Pico;

/* A zero start_frame selects root frame 1, the original opening in this game. */
int pico_init(Pico *p, const PicoData *data, const PicoHost *host, uint16_t start_frame);
void pico_tick(Pico *p);
void pico_render(const Pico *p);
/* Pointer coordinates are stage pixels in signed 16.16. down is boolean. */
void pico_pointer(Pico *p, int32_t x, int32_t y, int down);
/* Query the topmost actionable button instance, or PICO_NONE. Coordinates use
 * the same stage space as pico_pointer. No events are dispatched and pointer,
 * hover and capture state are unchanged. Malformed data may set error flags. */
uint16_t pico_hit_test(Pico *p, int32_t x, int32_t y);
uint16_t pico_find(const Pico *p, uint16_t from, const char *path);
uint16_t pico_frame(const Pico *p, const char *absolute_path);
/* Tooling/control interface uses the same operations as compiled scripts. */
void pico_goto(Pico *p, const char *absolute_path, uint16_t frame, int play);
void pico_play(Pico *p, const char *absolute_path);
void pico_stop(Pico *p, const char *absolute_path);
void pico_next(Pico *p, const char *absolute_path);
/* Invoke a visible button by instance handle, for deterministic host tests. */
void pico_button(Pico *p, uint16_t instance, int press);
typedef void (*PicoButtonVisitor)(void *user, uint16_t instance, uint16_t symbol,
                                  const PicoMatrix *world);
/* Back-to-front actionable buttons; a host may cycle these for tiny screens.
 * Visitor can read p->instances[instance].generation to guard saved handles. */
void pico_buttons(const Pico *p, PicoButtonVisitor visit, void *user);
void pico_matrix_compose(PicoMatrix *out, const PicoMatrix *parent, const PicoMatrix *local);

#endif
