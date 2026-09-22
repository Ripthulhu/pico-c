#define _POSIX_C_SOURCE 200809L
#include "host_common.h"
#ifdef PICO_HAVE_PULSE
#include "audio_pulse.h"
#endif
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static HostApp app;

typedef struct Presentation {
    XImage *image;
    unsigned scale, width, height;
    int x, y;
} Presentation;

static int resize_presentation(Display *display, int screen, Presentation *view, unsigned width,
                               unsigned height, unsigned scale) {
    unsigned logical_width, logical_height, image_width, image_height;
    XImage *image;
    if (!width || !height)
        return 1;
#ifdef PICO_STREAM_RENDER
    logical_width = app.width;
    logical_height = app.height;
    scale = width / logical_width;
    if (scale > height / logical_height)
        scale = height / logical_height;
    if (!scale)
        scale = 1;
    if (scale > 16)
        scale = 16;
#else
    logical_width = width / scale;
    logical_height = height / scale;
    if (!logical_width)
        logical_width = 1;
    if (!logical_height)
        logical_height = 1;
    if (logical_width > PICO_RENDER_MAX_WIDTH)
        logical_width = PICO_RENDER_MAX_WIDTH;
    if (logical_height > PICO_HOST_MAX_HEIGHT)
        logical_height = PICO_HOST_MAX_HEIGHT;
#endif
    image_width = logical_width * scale;
    image_height = logical_height * scale;
    if (!view->image || image_width != (unsigned)view->image->width ||
        image_height != (unsigned)view->image->height) {
        image = XCreateImage(display, DefaultVisual(display, screen), DefaultDepth(display, screen),
                             ZPixmap, 0, NULL, image_width, image_height, 32, 0);
        if (!image)
            return 0;
        image->data = (char *)calloc((size_t)image->bytes_per_line * image_height, 1);
        if (!image->data) {
            XDestroyImage(image);
            return 0;
        }
        /* Keep the current buffers until both replacement allocations succeed. */
        if (!host_resize(&app, logical_width, logical_height)) {
            XDestroyImage(image);
            return 0;
        }
        /* Pointer events later in this batch need the new game viewport. */
        if (!host_render(&app)) {
            XDestroyImage(image);
            return 0;
        }
        if (view->image)
            XDestroyImage(view->image);
        view->image = image;
    }
    view->scale = scale;
    view->width = width;
    view->height = height;
    view->x = ((int)width - (int)image_width) / 2;
    view->y = ((int)height - (int)image_height) / 2;
    return 1;
}

static unsigned long channel(unsigned value, unsigned long mask) {
    unsigned shift = 0;
    unsigned long scaled;
    if (!mask)
        return 0;
    while (!(mask & 1)) {
        mask >>= 1;
        shift++;
    }
    scaled = (value * mask + 127) / 255;
    return scaled << shift;
}

static void present(Display *display, Window window, GC gc, const Presentation *view) {
    static char previous_title[256];
    unsigned x, y, dx, dy;
    unsigned scale = view->scale;
    XImage *image = view->image;
    char caption[128], title[256];
    int right = view->x + image->width, bottom = view->y + image->height;
    for (y = 0; y < app.height; y++) {
        for (x = 0; x < app.width; x++) {
            uint32_t pixel = host_pixel_rgb(&app, (size_t)y * app.width + x);
            unsigned r = (pixel >> 16) & 255;
            unsigned g = (pixel >> 8) & 255;
            unsigned b = pixel & 255;
            unsigned long native = channel(r, image->red_mask) | channel(g, image->green_mask) |
                                   channel(b, image->blue_mask);
            for (dy = 0; dy < scale; dy++)
                for (dx = 0; dx < scale; dx++)
                    XPutPixel(image, (int)(x * scale + dx), (int)(y * scale + dy), native);
        }
    }
    host_caption(&app, app.selected, caption, sizeof(caption));
    snprintf(title, sizeof(title), "Pico Tiny %ux%u%s | %s | Tab / Space / T read", app.width,
             app.height, app.mono ? " mono" : "", caption);
    if (strcmp(title, previous_title)) {
        XStoreName(display, window, title);
        memcpy(previous_title, title, strlen(title) + 1);
    }
    /* Leave the previous game image visible until its replacement is sent. */
    if (view->x > 0)
        XFillRectangle(display, window, gc, 0, 0, (unsigned)view->x, view->height);
    if (right < (int)view->width)
        XFillRectangle(display, window, gc, right, 0, view->width - (unsigned)right, view->height);
    if (view->y > 0)
        XFillRectangle(display, window, gc, 0, 0, view->width, (unsigned)view->y);
    if (bottom < (int)view->height)
        XFillRectangle(display, window, gc, 0, bottom, view->width,
                       view->height - (unsigned)bottom);
    XPutImage(display, window, gc, image, 0, 0, view->x, view->y, app.width * scale,
              app.height * scale);
    XFlush(display);
}

static void help(void) {
    fputs("pico-x11 [--assets FILE] [--width N --height N] [--scale N] [--mono]\n"
          "         [--root N] [--assist|--no-assist] [--auto-exit TICKS] [--ppm FILE]\n"
          "         [--mute]"
#ifdef PICO_HAVE_PULSE
          " [--sounds FILE]"
#endif
          "\n"
          "Mouse: point/click. Tab or Left/Right: choose action. Space/Enter: use.\n"
          "T: readable text overlay (pauses game). Space: next text page. Escape: close/quit.\n"
#ifdef PICO_STREAM_RENDER
          "Resize: enlarge the fixed game buffer by whole pixels.\n",
#else
          "Resize: change the game buffer size; --scale is its pixel enlargement.\n",
#endif
          stderr);
}

int main(int argc, char **argv) {
    const char *assets = PICO_DEFAULT_ASSETS, *ppm = NULL;
#ifdef PICO_HAVE_PULSE
    const char *sounds = PICO_DEFAULT_SOUNDS;
    HostAudioLive *audio = NULL;
#endif
    HostSoundSink sound_sink = {0};
    unsigned width = PICO_DEFAULT_WIDTH, height = PICO_DEFAULT_HEIGHT;
#ifdef PICO_STREAM_RENDER
    unsigned scale = 4;
#else
    unsigned scale = 2;
#endif
    unsigned root = 1, auto_exit = 0;
    int mono = 0, assist = -1, i, running = 1, down = 0, dirty = 1, failed = 0, mute = 0;
    Display *display;
    int screen;
    Window window;
    GC gc;
    Presentation view = {NULL, 1, 0, 0, 0, 0};
    XSizeHints hints;
    Atom delete_window;
    double next_tick, next_frame;
    unsigned was_reading = 0;
    for (i = 1; i < argc; i++) {
        const char *option = argv[i];
        if (!strcmp(option, "--mute")) {
            mute = 1;
            continue;
        }
        if (!strcmp(option, "--mono")) {
            mono = 1;
            continue;
        }
        if (!strcmp(option, "--assist")) {
            assist = 1;
            continue;
        }
        if (!strcmp(option, "--no-assist")) {
            assist = 0;
            continue;
        }
        if (!strcmp(option, "--help")) {
            help();
            return 0;
        }
        if (++i == argc) {
            help();
            return 2;
        }
        if (!strcmp(option, "--assets"))
            assets = argv[i];
        else if (!strcmp(option, "--ppm"))
            ppm = argv[i];
#ifdef PICO_HAVE_PULSE
        else if (!strcmp(option, "--sounds"))
            sounds = argv[i];
#endif
        else {
            char *end;
            unsigned long n = strtoul(argv[i], &end, 10);
            if (!argv[i][0] || *end || n > 100000000u) {
                help();
                return 2;
            }
            if (!strcmp(option, "--width"))
                width = (unsigned)n;
            else if (!strcmp(option, "--height"))
                height = (unsigned)n;
            else if (!strcmp(option, "--scale"))
                scale = (unsigned)n;
            else if (!strcmp(option, "--root"))
                root = (unsigned)n;
            else if (!strcmp(option, "--auto-exit"))
                auto_exit = (unsigned)n;
            else {
                help();
                return 2;
            }
        }
    }
    if (!scale || scale > 16 || !root || root > 65535 || width > 8192 / scale ||
        height > 8192 / scale) {
        help();
        return 2;
    }
#ifdef PICO_HAVE_PULSE
    if (!mute) {
        audio = host_audio_live_open(sounds);
        if (audio)
            sound_sink = host_audio_live_sink(audio);
        else
            fputs("Continuing without audio.\n", stderr);
    }
#else
    (void)mute;
#endif
    if (!host_open_with_sound(&app, assets, width, height, root, mono, &sound_sink)) {
#ifdef PICO_HAVE_PULSE
        host_audio_live_close(audio);
#endif
        return 2;
    }
    app.assist = assist < 0 ? width <= 128 : (unsigned)assist;
    app.assist_auto = assist < 0;
    if (height < 16)
        app.assist = 0;
    host_select(&app, 0);
    display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "Cannot open X11 display; use pico-headless or enable WSLg.\n");
        host_close(&app);
#ifdef PICO_HAVE_PULSE
        host_audio_live_close(audio);
#endif
        return 2;
    }
    screen = DefaultScreen(display);
    window = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, width * scale,
                                 height * scale, 0, 0, 0);
    XSelectInput(display, window,
                 ExposureMask | KeyPressMask | PointerMotionMask | ButtonPressMask |
                     ButtonReleaseMask | StructureNotifyMask);
    memset(&hints, 0, sizeof(hints));
    hints.flags = PMinSize | PMaxSize;
#ifdef PICO_STREAM_RENDER
    hints.min_width = (int)width;
    hints.min_height = (int)height;
    hints.max_width = width > 512 ? 8192 : (int)(width * 16);
    hints.max_height = height > 512 ? 8192 : (int)(height * 16);
#else
    hints.min_width = (int)((width < 64 ? width : 64) * scale);
    hints.min_height = (int)((height < 32 ? height : 32) * scale);
    hints.max_width =
        PICO_RENDER_MAX_WIDTH > 8192 / scale ? 8192 : (int)(PICO_RENDER_MAX_WIDTH * scale);
    hints.max_height =
        PICO_HOST_MAX_HEIGHT > 8192 / scale ? 8192 : (int)(PICO_HOST_MAX_HEIGHT * scale);
#endif
    XSetWMNormalHints(display, window, &hints);
    delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &delete_window, 1);
    gc = XCreateGC(display, window, 0, NULL);
    if (!resize_presentation(display, screen, &view, width * scale, height * scale, scale)) {
        fprintf(stderr, "Cannot create display image.\n");
        XFreeGC(display, gc);
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        host_close(&app);
#ifdef PICO_HAVE_PULSE
        host_audio_live_close(audio);
#endif
        return 2;
    }
    XMapWindow(display, window);
#ifdef PICO_HAVE_PULSE
    if (audio && !host_audio_live_start(audio)) {
        fputs("Cannot start audio output thread.\n", stderr);
        host_audio_live_close(audio);
        audio = NULL;
        memset(&app.sound, 0, sizeof(app.sound));
    }
#endif
    next_frame = host_now();
    next_tick = next_frame + 1.0 / 24;
    while (running) {
        unsigned events = 0;
        while (events++ < 16 && XPending(display)) {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == ClientMessage && (Atom)event.xclient.data.l[0] == delete_window)
                running = 0;
            else if (event.type == DestroyNotify)
                running = 0;
            else if (event.type == Expose)
                dirty = 1;
            else if (event.type == ConfigureNotify) {
                XEvent latest;
                /* A drag can queue several sizes before the next frame is drawn. */
                while (XCheckTypedWindowEvent(display, window, ConfigureNotify, &latest))
                    event = latest;
                if (!resize_presentation(display, screen, &view, (unsigned)event.xconfigure.width,
                                         (unsigned)event.xconfigure.height, scale)) {
                    fprintf(stderr, "Cannot resize display image.\n");
                    failed = 1;
                    running = 0;
                }
                dirty = 1;
            } else if (event.type == KeyPress) {
                KeySym key = XLookupKeysym(&event.xkey, 0);
                if (key == XK_Escape) {
                    if (app.reading)
                        app.reading = 0;
                    else
                        running = 0;
                } else if (key == XK_t || key == XK_T) {
                    app.reading = !app.reading;
                    app.reader_page = 0;
                } else if (key == XK_space || key == XK_Return) {
                    if (app.reading) {
                        unsigned pages = host_reader_pages(&app);
                        app.reader_page = pages ? (app.reader_page + 1) % pages : 0;
                    } else
                        host_activate(&app);
                } else if (key == XK_Tab || key == XK_Right || key == XK_Down)
                    host_select(&app, 1);
                else if (key == XK_Left || key == XK_Up)
                    host_select(&app, -1);
                dirty = 1;
            } else if (!app.reading && (event.type == MotionNotify || event.type == ButtonPress ||
                                        event.type == ButtonRelease)) {
                int x = event.type == MotionNotify ? event.xmotion.x : event.xbutton.x;
                int y = event.type == MotionNotify ? event.xmotion.y : event.xbutton.y;
                if (event.type == ButtonPress && event.xbutton.button == Button1)
                    down = 1;
                if (event.type == ButtonRelease && event.xbutton.button == Button1)
                    down = 0;
                if (app.view_width && app.view_height) {
                    int64_t game_x = (int64_t)x - view.x - app.view_x * view.scale;
                    int64_t game_y = (int64_t)y - view.y - app.view_y * view.scale;
                    unsigned game_width = app.view_width * view.scale;
                    unsigned game_height = app.view_height * view.scale;
                    if (game_x < 0 || game_y < 0 || game_x >= game_width || game_y >= game_height)
                        pico_pointer(&app.game, -65536, -65536, down);
                    else
                        pico_pointer(&app.game, (int32_t)(game_x * 550 * 65536 / game_width),
                                     (int32_t)(game_y * 350 * 65536 / game_height), down);
                }
                dirty = 1;
            }
        }
        if (failed)
            break;
#ifdef PICO_HAVE_PULSE
        host_audio_live_pause(audio, app.reading != 0);
#endif
        {
            double now = host_now();
            unsigned ticks = 0;
            if (app.reading || was_reading != app.reading)
                next_tick = now + 1.0 / 24;
            was_reading = app.reading;
            while (!app.reading && now >= next_tick && ticks < 8) {
                host_tick(&app);
                next_tick += 1.0 / 24;
                ticks++;
                dirty = 1;
                if (auto_exit && app.game.tick >= auto_exit) {
                    running = 0;
                    break;
                }
            }
            if (ticks)
                host_select(&app, 0);
        }
        /* Service input between bounded catch-up passes. Slow drawing must
         * not discard game ticks or queue pictures of obsolete game state. */
        if (app.game.errors || app.draw_overflows)
            running = 0;
        if (running && !app.reading && host_now() >= next_tick)
            continue;
        if (dirty && (!running || host_now() >= next_frame)) {
            next_frame = host_now() + 1.0 / 24;
            if (!host_render(&app)) {
                fprintf(stderr, "Rendering failed.\n");
                failed = 1;
                running = 0;
            } else
                present(display, window, gc, &view);
            dirty = 0;
        }
        {
            struct timespec pause = {0, 2000000};
            nanosleep(&pause, NULL);
        }
    }
    if (ppm && !host_ppm(&app, ppm))
        fprintf(stderr, "Cannot save %s.\n", ppm);
#ifdef PICO_HAVE_PULSE
    host_audio_live_close(audio);
#endif
    host_metrics(&app, stdout);
    XDestroyImage(view.image);
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    i = failed || app.game.errors || app.draw_overflows ? 1 : 0;
    host_close(&app);
    return i;
}
