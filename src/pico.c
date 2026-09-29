#include "pico.h"
#include <limits.h>
#include <string.h>

static const PicoMatrix identity = PICO_IDENTITY;
static const PicoColor color_identity = PICO_COLOR_IDENTITY;

static int32_t clamp32(int64_t n) {
    return n > INT32_MAX ? INT32_MAX : n < INT32_MIN ? INT32_MIN : (int32_t)n;
}
static int16_t clamp16(int32_t n) {
    return n > INT16_MAX ? INT16_MAX : n < INT16_MIN ? INT16_MIN : (int16_t)n;
}
void pico_matrix_compose(PicoMatrix *out, const PicoMatrix *p, const PicoMatrix *q) {
    PicoMatrix r;
    r.a = clamp32(((int64_t)p->a * q->a + (int64_t)p->c * q->b) / 65536);
    r.b = clamp32(((int64_t)p->b * q->a + (int64_t)p->d * q->b) / 65536);
    r.c = clamp32(((int64_t)p->a * q->c + (int64_t)p->c * q->d) / 65536);
    r.d = clamp32(((int64_t)p->b * q->c + (int64_t)p->d * q->d) / 65536);
    r.tx = clamp32(((int64_t)p->a * q->tx + (int64_t)p->c * q->ty) / 65536 + p->tx);
    r.ty = clamp32(((int64_t)p->b * q->tx + (int64_t)p->d * q->ty) / 65536 + p->ty);
    *out = r;
}
static void color_compose(PicoColor *out, const PicoColor *p, const PicoColor *q) {
    unsigned i;
    for (i = 0; i < 4; i++) {
        out->mul[i] = clamp16((int32_t)p->mul[i] * q->mul[i] / 256);
        out->add[i] = clamp16((int32_t)p->mul[i] * q->add[i] / 256 + p->add[i]);
    }
}
static int live(const Pico *p, uint16_t n) {
    return n < PICO_MAX_INSTANCES && p->instances[n].alive;
}
static const char *str(const Pico *p, uint16_t id) {
    return id < p->data->string_count ? p->data->strings[id] : "";
}
static const PicoPlacement *placement(const Pico *p, uint16_t n) {
    return &p->data->placements[p->instances[n].placement];
}
static const PicoColor *placement_color(const Pico *p, const PicoPlacement *pl) {
    return pl->color < p->data->color_count ? &p->data->colors[pl->color] : &color_identity;
}
static const PicoFrame *frame_at(Pico *p, uint16_t symbol, uint16_t frame) {
    const PicoSymbol *s;
    uint32_t i;
    if (symbol >= p->data->symbol_count || frame == 0)
        goto bad;
    s = &p->data->symbols[symbol];
    if (frame > s->frame_count)
        goto bad;
    i = s->first_frame + frame - 1;
    if (i >= p->data->frame_count)
        goto bad;
    return &p->data->frames[i];
bad:
    p->errors |= PICO_ERROR_BAD_DATA;
    return 0;
}
static void enqueue(Pico *p, uint16_t owner, uint32_t first, uint16_t count) {
    PicoQueued *q;
    if (!count)
        return;
    if (first > p->data->action_count || count > p->data->action_count - first) {
        p->errors |= PICO_ERROR_BAD_DATA;
        return;
    }
    if (p->queue_count == PICO_MAX_QUEUE) {
        p->errors |= PICO_ERROR_QUEUE_LIMIT;
        return;
    }
    q = &p->queue[(p->queue_head + p->queue_count) % PICO_MAX_QUEUE];
    q->first = first;
    q->count = count;
    q->owner = owner;
    q->generation = p->instances[owner].generation;
    q->revision = p->instances[owner].revision;
    p->queue_count++;
    if (p->queue_count > p->peak_queue)
        p->peak_queue = p->queue_count;
}
static void destroy(Pico *p, uint16_t n, unsigned level) {
    uint16_t c, next;
    if (!live(p, n))
        return;
    if (level > PICO_MAX_NESTING) {
        p->errors |= PICO_ERROR_NESTING;
        return;
    }
    c = p->instances[n].child;
    while (c != PICO_NONE) {
        next = p->instances[c].next;
        destroy(p, c, level + 1);
        c = next;
    }
    p->instances[n].alive = 0;
    p->live_instances--;
}
static uint16_t allocate(Pico *p, uint16_t parent, uint32_t ref) {
    uint16_t n;
    PicoInstance *i;
    const PicoPlacement *pl = &p->data->placements[ref];
    if (pl->symbol >= p->data->symbol_count) {
        p->errors |= PICO_ERROR_BAD_DATA;
        return PICO_NONE;
    }
    for (n = 1; n < PICO_MAX_INSTANCES; n++)
        if (!p->instances[n].alive)
            break;
    if (n == PICO_MAX_INSTANCES) {
        p->errors |= PICO_ERROR_INSTANCE_LIMIT;
        return PICO_NONE;
    }
    i = &p->instances[n];
    memset(i, 0, sizeof(*i));
    i->placement = ref;
    i->symbol = pl->symbol;
    i->parent = parent;
    i->child = i->next = PICO_NONE;
    i->alive = 1;
    i->playing = p->data->symbols[i->symbol].kind == PICO_MOVIE;
    i->born_tick = p->tick;
    i->generation = ++p->next_generation;
    if (!i->generation)
        i->generation = ++p->next_generation;
    p->live_instances++;
    if (p->live_instances > p->peak_instances)
        p->peak_instances = p->live_instances;
    return n;
}

static void enter(Pico *p, uint16_t n, uint16_t frame, unsigned level);
static void synchronize(Pico *p, uint16_t n, const PicoFrame *f, unsigned level) {
    uint16_t child, previous = PICO_NONE, next, j;
    uint32_t ref;
    int found;
    if (level >= PICO_MAX_NESTING) {
        p->errors |= PICO_ERROR_NESTING;
        return;
    }
    if (f->first_placement > p->data->placement_ref_count ||
        f->placement_count > p->data->placement_ref_count - f->first_placement) {
        p->errors |= PICO_ERROR_BAD_DATA;
        return;
    }
    for (j = 0; j < f->placement_count; j++) {
        ref = p->data->placement_refs[f->first_placement + j];
        if (ref >= p->data->placement_count) {
            p->errors |= PICO_ERROR_BAD_DATA;
            return;
        }
        if (p->data->placements[ref].color >= p->data->color_count ||
            p->data->placements[ref].clip_depth) {
            p->errors |= PICO_ERROR_BAD_DATA;
            return;
        }
    }
    /* Remove only vanished lifetimes. Controllers surviving a room transition
     * retain both their playheads and their descendants. */
    child = p->instances[n].child;
    while (child != PICO_NONE) {
        const PicoPlacement *old = placement(p, child);
        next = p->instances[child].next;
        found = 0;
        for (j = 0; j < f->placement_count; j++) {
            ref = p->data->placement_refs[f->first_placement + j];
            if (p->data->placements[ref].life == old->life &&
                p->data->placements[ref].symbol == old->symbol &&
                p->data->placements[ref].depth == old->depth) {
                found = 1;
                break;
            }
        }
        if (!found) {
            if (previous == PICO_NONE)
                p->instances[n].child = next;
            else
                p->instances[previous].next = next;
            destroy(p, child, level + 1);
        } else
            previous = child;
        child = next;
    }
    /* Build every direct sibling before recursively initializing children, so
     * absolute tellTarget references can see all controllers on first entry. */
    for (j = 0; j < f->placement_count; j++) {
        const PicoPlacement *want;
        uint8_t kind;
        ref = p->data->placement_refs[f->first_placement + j];
        want = &p->data->placements[ref];
        if (want->symbol >= p->data->symbol_count) {
            p->errors |= PICO_ERROR_BAD_DATA;
            continue;
        }
        kind = p->data->symbols[want->symbol].kind;
        /* Shapes, text and bitmaps have no changing runtime state. They are
         * drawn directly from the current ROM snapshot, without a RAM slot. */
        if (kind != PICO_MOVIE && kind != PICO_BUTTON)
            continue;
        child = p->instances[n].child;
        while (child != PICO_NONE) {
            const PicoPlacement *old = placement(p, child);
            if (old->depth == want->depth && old->life == want->life && old->symbol == want->symbol)
                break;
            child = p->instances[child].next;
        }
        if (child == PICO_NONE) {
            uint16_t before = PICO_NONE, c = p->instances[n].child;
            child = allocate(p, n, ref);
            if (child == PICO_NONE)
                continue;
            while (c != PICO_NONE && placement(p, c)->depth < want->depth) {
                before = c;
                c = p->instances[c].next;
            }
            p->instances[child].next = c;
            if (before == PICO_NONE)
                p->instances[n].child = child;
            else
                p->instances[before].next = child;
        } else
            p->instances[child].placement = ref;
    }
    child = p->instances[n].child;
    while (child != PICO_NONE) {
        next = p->instances[child].next;
        if (!p->instances[child].frame) {
            const PicoSymbol *s = &p->data->symbols[p->instances[child].symbol];
            if ((s->kind == PICO_MOVIE || s->kind == PICO_BUTTON) && s->frame_count)
                enter(p, child, 1, level + 1);
            else
                p->instances[child].frame = 1;
        }
        child = next;
    }
}
static void frame_sounds(Pico *p, const PicoFrame *f) {
    uint16_t j;
    if (f->first_sound > p->data->sound_count ||
        f->sound_count > p->data->sound_count - f->first_sound) {
        p->errors |= PICO_ERROR_BAD_DATA;
        return;
    }
    for (j = 0; j < f->sound_count; j++) {
        const PicoSoundEvent *s = &p->data->sounds[f->first_sound + j];
        if (s->envelope_count && !s->envelope) {
            p->errors |= PICO_ERROR_BAD_DATA;
            continue;
        }
        if (p->host.sound)
            p->host.sound(p->host.user, s);
    }
}

static void enter(Pico *p, uint16_t n, uint16_t frame, unsigned level) {
    const PicoFrame *f;
    if (!live(p, n))
        return;
    if (level >= PICO_MAX_NESTING) {
        p->errors |= PICO_ERROR_NESTING;
        return;
    }
    if (p->instances[n].frame == frame)
        return;
    f = frame_at(p, p->instances[n].symbol, frame);
    if (!f)
        return;
    p->instances[n].frame = frame;
    p->instances[n].revision++;
    /* Parent scripts are queued before new descendants' scripts. A subsequent
     * jump invalidates obsolete pending scripts by the frame revision. */
    enqueue(p, n, f->first_action, f->action_count);
    synchronize(p, n, f, level);
    if (p->data->symbols[p->instances[n].symbol].kind != PICO_BUTTON)
        frame_sounds(p, f);
}

static int token_equal(const char *name, const char *token, size_t length) {
    return strlen(name) == length && !memcmp(name, token, length);
}
uint16_t pico_find(const Pico *p, uint16_t from, const char *path) {
    const char *start;
    size_t length;
    uint16_t child;
    if (!p || !p->data || !path)
        return PICO_NONE;
    if (*path == '/') {
        from = 0;
        while (*path == '/')
            path++;
    }
    if (!strncmp(path, "_root", 5) && (path[5] == 0 || path[5] == '/' || path[5] == '.')) {
        from = 0;
        path += 5;
        if (*path)
            path++;
    }
    if (!live(p, from))
        return PICO_NONE;
    while (*path) {
        while (*path == '/')
            path++;
        if (!*path)
            break;
        start = path;
        if (path[0] == '.' && path[1] == '.' && (path[2] == '/' || path[2] == 0)) {
            if (from)
                from = p->instances[from].parent;
            path += 2;
            continue;
        }
        while (*path && *path != '/' && *path != '.')
            path++;
        length = (size_t)(path - start);
        if (!length) {
            path++;
            continue;
        }
        if (token_equal("this", start, length))
            continue;
        if (token_equal("_parent", start, length)) {
            if (from)
                from = p->instances[from].parent;
        } else if (token_equal("_root", start, length))
            from = 0;
        else {
            child = p->instances[from].child;
            while (child != PICO_NONE &&
                   !token_equal(str(p, placement(p, child)->name), start, length))
                child = p->instances[child].next;
            if (child == PICO_NONE)
                return PICO_NONE;
            from = child;
        }
        if (*path == '.')
            path++;
    }
    return from;
}
static void go(Pico *p, uint16_t n, uint16_t frame, int play) {
    const PicoSymbol *s;
    if (!live(p, n))
        return;
    s = &p->data->symbols[p->instances[n].symbol];
    if (s->kind != PICO_MOVIE || !s->frame_count)
        return;
    if (frame < 1)
        frame = 1;
    if (frame > s->frame_count)
        frame = s->frame_count;
    p->instances[n].playing = (uint8_t)(play != 0);
    enter(p, n, frame, 0);
}
static void drain(Pico *p) {
    uint32_t executed = 0;
    if (p->draining)
        return;
    p->draining = 1;
    while (p->queue_count) {
        PicoQueued q = p->queue[p->queue_head];
        uint16_t j;
        p->queue_head = (uint16_t)((p->queue_head + 1) % PICO_MAX_QUEUE);
        p->queue_count--;
        if (!live(p, q.owner) || p->instances[q.owner].generation != q.generation ||
            p->instances[q.owner].revision != q.revision)
            continue;
        for (j = 0; j < q.count; j++) {
            const PicoAction *a = &p->data->actions[q.first + j];
            uint16_t target = PICO_NONE;
            if (++executed > PICO_MAX_ACTIONS_PER_EVENT) {
                p->errors |= PICO_ERROR_ACTION_LIMIT;
                p->queue_count = 0;
                break;
            }
            p->actions_executed++;
            if (a->op >= PICO_PLAY && a->op <= PICO_LABEL_STOP) {
                /* A running script can remove its own clip while changing a
                 * parent frame. Finish its absolute actions, but never resolve
                 * relative paths through a removed or recycled owner slot. */
                uint16_t owner =
                    live(p, q.owner) && p->instances[q.owner].generation == q.generation
                        ? q.owner
                        : PICO_NONE;
                target = pico_find(p, owner, str(p, a->target));
                if (target == PICO_NONE) {
                    p->missing_targets++;
                    continue;
                }
            }
            switch (a->op) {
            case PICO_NOP:
                break;
            case PICO_PLAY:
                p->instances[target].playing = 1;
                break;
            case PICO_STOP:
                p->instances[target].playing = 0;
                break;
            case PICO_NEXT:
                go(p, target, (uint16_t)(p->instances[target].frame + 1), 0);
                break;
            case PICO_GOTO_PLAY:
            case PICO_GOTO_STOP:
                go(p, target, a->arg, a->op == PICO_GOTO_PLAY);
                break;
            case PICO_LABEL_PLAY:
            case PICO_LABEL_STOP: {
                const PicoSymbol *s = &p->data->symbols[p->instances[target].symbol];
                uint16_t f;
                for (f = 1; f <= s->frame_count; f++) {
                    const PicoFrame *fr = frame_at(p, p->instances[target].symbol, f);
                    if (fr && fr->label == a->arg)
                        break;
                }
                if (f > s->frame_count)
                    p->missing_labels++;
                else
                    go(p, target, f, a->op == PICO_LABEL_PLAY);
                break;
            }
            case PICO_STOP_SOUNDS:
                if (p->host.stop_sounds)
                    p->host.stop_sounds(p->host.user);
                break;
            case PICO_MEDAL:
                if (p->host.medal)
                    p->host.medal(p->host.user, str(p, a->arg));
                break;
            case PICO_SET_FLAG:
                if (a->arg < 32)
                    p->flags |= (uint32_t)1 << a->arg;
                else
                    p->errors |= PICO_ERROR_BAD_DATA;
                break;
            case PICO_IF_FLAG_SKIP:
            case PICO_IF_ROOT_NOT_FRAME_SKIP:
                if (a->op == PICO_IF_ROOT_NOT_FRAME_SKIP
                        ? p->instances[0].frame != a->aux
                        : a->aux < 32 && (p->flags & ((uint32_t)1 << a->aux))) {
                    if (a->arg > q.count - j - 1)
                        j = q.count - 1;
                    else
                        j = (uint16_t)(j + a->arg);
                }
                break;
            default:
                p->errors |= PICO_ERROR_BAD_DATA;
                break;
            }
        }
    }
    p->draining = 0;
}
int pico_init(Pico *p, const PicoData *data, const PicoHost *host, uint16_t start_frame) {
    PicoInstance *root;
    if (!p)
        return 0;
    memset(p, 0, sizeof(*p));
    p->data = data;
    p->pressed = p->hover = PICO_NONE;
    if (!data || !data->symbols || !data->frames || !data->strings || !data->symbol_count ||
        !data->string_count || data->symbols[0].kind != PICO_MOVIE ||
        (data->placement_count && (!data->placements || !data->colors || !data->color_count)) ||
        (data->placement_ref_count && !data->placement_refs) ||
        (data->action_count && !data->actions) || (data->sound_count && !data->sounds)) {
        p->errors = PICO_ERROR_BAD_DATA;
        return 0;
    }
    if (host)
        p->host = *host;
    root = &p->instances[0];
    root->alive = root->playing = 1;
    root->generation = 1;
    root->parent = root->child = root->next = PICO_NONE;
    p->live_instances = p->peak_instances = 1;
    p->next_generation = 1;
    go(p, 0, start_frame ? start_frame : 1, 1);
    drain(p);
    return p->errors == 0;
}
void pico_tick(Pico *p) {
    uint16_t n;
    p->tick++;
    for (n = 0; n < PICO_MAX_INSTANCES; n++)
        if (live(p, n) && p->instances[n].playing && p->instances[n].born_tick != p->tick) {
            const PicoSymbol *s = &p->data->symbols[p->instances[n].symbol];
            if (s->kind == PICO_MOVIE && s->frame_count > 1) {
                uint16_t f = (uint16_t)(p->instances[n].frame + 1);
                if (f > s->frame_count)
                    f = 1;
                enter(p, n, f, 0);
            }
        }
    drain(p);
}
uint16_t pico_frame(const Pico *p, const char *path) {
    uint16_t n = pico_find(p, 0, path);
    return n == PICO_NONE ? 0 : p->instances[n].frame;
}
void pico_goto(Pico *p, const char *path, uint16_t frame, int play) {
    uint16_t n = pico_find(p, 0, path);
    if (n == PICO_NONE) {
        p->missing_targets++;
        return;
    }
    go(p, n, frame, play);
    drain(p);
}
void pico_play(Pico *p, const char *path) {
    uint16_t n = pico_find(p, 0, path);
    if (n == PICO_NONE)
        p->missing_targets++;
    else
        p->instances[n].playing = 1;
}
void pico_stop(Pico *p, const char *path) {
    uint16_t n = pico_find(p, 0, path);
    if (n == PICO_NONE)
        p->missing_targets++;
    else
        p->instances[n].playing = 0;
}
void pico_next(Pico *p, const char *path) {
    uint16_t n = pico_find(p, 0, path);
    if (n == PICO_NONE) {
        p->missing_targets++;
        return;
    }
    go(p, n, (uint16_t)(p->instances[n].frame + 1), 0);
    drain(p);
}
void pico_button(Pico *p, uint16_t instance, int press) {
    const PicoSymbol *s;
    if (!live(p, instance))
        return;
    s = &p->data->symbols[p->instances[instance].symbol];
    if (s->kind != PICO_BUTTON || p->instances[instance].parent == PICO_NONE)
        return;
    /* Sound belongs to the press transition. Dragging back onto a held button
     * restores its down artwork without starting another shot. */
    if (s->frame_count >= (press ? 3 : 2))
        enter(p, instance, (uint16_t)(press ? 3 : 2), 0);
    if (press && s->frame_count >= 3) {
        const PicoFrame *f = frame_at(p, p->instances[instance].symbol, 3);
        if (f)
            frame_sounds(p, f);
    }
    enqueue(p, p->instances[instance].parent, press ? s->first_press : s->first_release,
            press ? s->press_count : s->release_count);
    drain(p);
}

static void render_node(const Pico *p, uint16_t n, const PicoMatrix *parent,
                        const PicoColor *parent_color, unsigned level) {
    PicoMatrix world;
    PicoColor color;
    const PicoPlacement *pl;
    const PicoSymbol *s;
    const PicoFrame *f;
    uint16_t j;
    uint32_t frame;
    if (level >= PICO_MAX_NESTING || !live(p, n))
        return;
    if (n == 0) {
        world = *parent;
        color = *parent_color;
    } else {
        pl = placement(p, n);
        pico_matrix_compose(&world, parent, &pl->matrix);
        color_compose(&color, parent_color, placement_color(p, pl));
    }
    s = &p->data->symbols[p->instances[n].symbol];
    if (!p->instances[n].frame)
        return;
    frame = s->first_frame + p->instances[n].frame - 1;
    if (frame >= p->data->frame_count)
        return;
    f = &p->data->frames[frame];
    if (f->first_placement > p->data->placement_ref_count ||
        f->placement_count > p->data->placement_ref_count - f->first_placement)
        return;
    for (j = 0; j < f->placement_count; j++) {
        uint32_t ref = p->data->placement_refs[f->first_placement + j];
        if (ref >= p->data->placement_count)
            return;
        pl = &p->data->placements[ref];
        if (pl->symbol >= p->data->symbol_count)
            return;
        if (p->data->symbols[pl->symbol].kind == PICO_LEAF) {
            PicoMatrix leaf_world;
            PicoColor leaf_color;
            pico_matrix_compose(&leaf_world, &world, &pl->matrix);
            color_compose(&leaf_color, &color, placement_color(p, pl));
            p->host.draw(p->host.user, pl->symbol, &leaf_world, &leaf_color, pl->ratio,
                         PICO_DRAW_LEAF);
        } else {
            uint16_t child = p->instances[n].child;
            while (child != PICO_NONE) {
                const PicoPlacement *cp = placement(p, child);
                if (cp->depth == pl->depth && cp->life == pl->life && cp->symbol == pl->symbol) {
                    render_node(p, child, &world, &color, level + 1);
                    break;
                }
                child = p->instances[child].next;
            }
        }
    }
}
void pico_render(const Pico *p) {
    if (p && p->data && p->host.draw)
        render_node(p, 0, &identity, &color_identity, 0);
}
static int hit_symbol(Pico *p, uint16_t symbol, uint16_t frame, const PicoMatrix *world, int32_t x,
                      int32_t y, unsigned level) {
    const PicoSymbol *s;
    const PicoFrame *f;
    uint16_t j;
    if (level >= PICO_MAX_NESTING) {
        p->errors |= PICO_ERROR_NESTING;
        return 0;
    }
    if (symbol >= p->data->symbol_count) {
        p->errors |= PICO_ERROR_BAD_DATA;
        return 0;
    }
    s = &p->data->symbols[symbol];
    if (s->kind == PICO_LEAF)
        return p->host.hit(p->host.user, symbol, world, x, y);
    if (s->kind != PICO_MOVIE && s->kind != PICO_BUTTON)
        return 0;
    f = frame_at(p, symbol, frame);
    if (!f)
        return 0;
    if (f->first_placement > p->data->placement_ref_count ||
        f->placement_count > p->data->placement_ref_count - f->first_placement) {
        p->errors |= PICO_ERROR_BAD_DATA;
        return 0;
    }
    for (j = 0; j < f->placement_count; j++) {
        uint32_t ref = p->data->placement_refs[f->first_placement + j];
        const PicoPlacement *pl;
        PicoMatrix m;
        if (ref >= p->data->placement_count) {
            p->errors |= PICO_ERROR_BAD_DATA;
            return 0;
        }
        pl = &p->data->placements[ref];
        pico_matrix_compose(&m, world, &pl->matrix);
        if (hit_symbol(p, pl->symbol, 1, &m, x, y, level + 1))
            return 1;
    }
    return 0;
}
static uint16_t hit_node(Pico *p, uint16_t n, const PicoMatrix *parent, int32_t x, int32_t y,
                         unsigned level) {
    PicoMatrix world;
    uint16_t child, result = PICO_NONE;
    const PicoSymbol *s;
    if (level >= PICO_MAX_NESTING) {
        p->errors |= PICO_ERROR_NESTING;
        return PICO_NONE;
    }
    if (n == 0)
        world = *parent;
    else
        pico_matrix_compose(&world, parent, &placement(p, n)->matrix);
    s = &p->data->symbols[p->instances[n].symbol];
    if (s->kind == PICO_BUTTON && (s->press_count || s->release_count) &&
        hit_symbol(p, p->instances[n].symbol, s->frame_count >= 4 ? 4 : 1, &world, x, y, level + 1))
        result = n;
    child = p->instances[n].child;
    while (child != PICO_NONE) {
        uint16_t candidate = hit_node(p, child, &world, x, y, level + 1);
        if (candidate != PICO_NONE)
            result = candidate;
        child = p->instances[child].next;
    }
    return result;
}
uint16_t pico_hit_test(Pico *p, int32_t x, int32_t y) {
    if (!p || !p->data || !p->host.hit)
        return PICO_NONE;
    return hit_node(p, 0, &identity, x, y, 0);
}
static int same_button(const Pico *p, uint16_t n, uint32_t generation) {
    return live(p, n) && p->instances[n].generation == generation;
}
static void button_state(Pico *p, uint16_t n, uint32_t generation, uint16_t frame) {
    if (same_button(p, n, generation) &&
        p->data->symbols[p->instances[n].symbol].frame_count >= frame)
        enter(p, n, frame, 0);
}
void pico_pointer(Pico *p, int32_t x, int32_t y, int down) {
    uint16_t hit = pico_hit_test(p, x, y);
    uint32_t generation = hit == PICO_NONE ? 0 : p->instances[hit].generation;
    down = down != 0;
    if (p->hover != hit || p->hover_generation != generation) {
        button_state(p, p->hover, p->hover_generation, 1);
        p->hover = hit;
        p->hover_generation = generation;
    }
    if (down && !p->pointer_down) {
        p->pressed = hit;
        p->pressed_generation = generation;
        button_state(p, hit, generation, 3);
        if (hit != PICO_NONE)
            pico_button(p, hit, 1);
    } else if (!down && p->pointer_down) {
        uint16_t pressed = p->pressed;
        uint32_t pressed_generation = p->pressed_generation;
        p->pressed = PICO_NONE;
        button_state(p, pressed, pressed_generation, pressed == hit ? 2 : 1);
        if (pressed == hit && generation == pressed_generation &&
            same_button(p, pressed, pressed_generation))
            pico_button(p, pressed, 0);
    } else
        button_state(p, hit, generation,
                     down && hit == p->pressed && generation == p->pressed_generation ? 3 : 2);
    p->pointer_down = (uint8_t)down;
    drain(p);
}
static void buttons_node(const Pico *p, uint16_t n, const PicoMatrix *parent,
                         PicoButtonVisitor visit, void *user, unsigned level) {
    PicoMatrix world;
    const PicoSymbol *s;
    uint16_t child;
    if (level >= PICO_MAX_NESTING)
        return;
    if (n == 0)
        world = *parent;
    else
        pico_matrix_compose(&world, parent, &placement(p, n)->matrix);
    s = &p->data->symbols[p->instances[n].symbol];
    if (s->kind == PICO_BUTTON && (s->press_count || s->release_count))
        visit(user, n, p->instances[n].symbol, &world);
    child = p->instances[n].child;
    while (child != PICO_NONE) {
        buttons_node(p, child, &world, visit, user, level + 1);
        child = p->instances[child].next;
    }
}
void pico_buttons(const Pico *p, PicoButtonVisitor visit, void *user) {
    if (p && p->data && visit)
        buttons_node(p, 0, &identity, visit, user, 0);
}
