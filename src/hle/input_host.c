/*
 * input_host.c -- the host controller behind the replaced XAPI input.
 *
 * Adapted from doaxbv-re (https://github.com/NoRain211/doaxbv-re,
 * recomp-runtime/input_host_win32.c), GPL-3.0: XInput pad 0 and the keyboard,
 * mapped to the Xbox gamepad. Added here: RECOMP_FAKE_INPUT, the scripted
 * presses src/usb/usb_gamepad.c already understood, so an unattended run can
 * still get past "Press START"; and RECOMP_INPUT_SEQ, a one-shot timed
 * sequence that walks a menu path the same way every run.
 *
 * Which host device drives which of the four ports, and which key or pad
 * control drives each Xbox input, is src/input/input_bindings.c reading the
 * config file the input UI writes (`py -3 -m tools.input_ui`). With no config
 * file the defaults are what this file used to hard-code: XInput pad 0 plus
 * the keyboard on port 1 -- arrows = D-pad, Enter = START, Backspace = BACK,
 * Z = A, X = B, A = X, S = Y, Q = White, W = Black, E/R = triggers -- and
 * XInput pad N on port N.
 *
 * RECOMP_FAKE_INPUT and RECOMP_INPUT_SEQ still press buttons directly, on
 * port 1 only: they are a substitute for a person at the keyboard, so they
 * are deliberately unaffected by what that person bound.
 */
#include "input_host.h"
#include "input_bindings.h"

/* <windows.h> on Windows, the POSIX shim (GetTickCount64) elsewhere. */
#include "platform/xbox_winnt.h"
#include "xbox_memory_layout.h"     /* xbox_FpsSwapCount */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    XBOX_DPAD_UP = 0x0001u,
    XBOX_DPAD_DOWN = 0x0002u,
    XBOX_DPAD_LEFT = 0x0004u,
    XBOX_DPAD_RIGHT = 0x0008u,
    XBOX_START = 0x0010u,
    XBOX_BACK = 0x0020u,
    XBOX_LTHUMB = 0x0040u,
    XBOX_RTHUMB = 0x0080u,
};

/* RECOMP_FAKE_INPUT=start,a  presses each listed button in turn, one per
 * cycle; RECOMP_FAKE_INPUT_MS (default 500) is how long each press and each
 * gap lasts. Pressed and released rather than held: menus act on the edge. */
/* kind: 0 a button, 1 an analog button, 2 a stick pushed fully one way.
 * The sticks are here because a script that can only press buttons can reach
 * a menu but not a place in a level, and "walk to where it looks wrong" is
 * how a rendering bug gets reproduced without a person at the pad. The names
 * match the controls in the binding config (src/input/input_bindings.c). */
enum { FAKE_BUTTON, FAKE_ANALOG, FAKE_STICK };
enum { STICK_LX, STICK_LY, STICK_RX, STICK_RY };

static const struct { const char *name; int kind; unsigned value; int sign; } FAKE[] = {
    { "start", FAKE_BUTTON, XBOX_START, 0 },      { "back",  FAKE_BUTTON, XBOX_BACK, 0 },
    { "up",    FAKE_BUTTON, XBOX_DPAD_UP, 0 },    { "down",  FAKE_BUTTON, XBOX_DPAD_DOWN, 0 },
    { "left",  FAKE_BUTTON, XBOX_DPAD_LEFT, 0 },  { "right", FAKE_BUTTON, XBOX_DPAD_RIGHT, 0 },
    /* The binding config's names for the same buttons. Without them a script
     * written with those names stopped at the first one ("unknown button
     * 'dpad_up'") and every later step was silently dropped. */
    { "dpad_up",   FAKE_BUTTON, XBOX_DPAD_UP, 0 },   { "dpad_down",  FAKE_BUTTON, XBOX_DPAD_DOWN, 0 },
    { "dpad_left", FAKE_BUTTON, XBOX_DPAD_LEFT, 0 }, { "dpad_right", FAKE_BUTTON, XBOX_DPAD_RIGHT, 0 },
    { "a",     FAKE_ANALOG, HOST_ANALOG_A, 0 },   { "b",     FAKE_ANALOG, HOST_ANALOG_B, 0 },
    { "x",     FAKE_ANALOG, HOST_ANALOG_X, 0 },   { "y",     FAKE_ANALOG, HOST_ANALOG_Y, 0 },
    { "white", FAKE_ANALOG, HOST_ANALOG_WHITE, 0 }, { "black", FAKE_ANALOG, HOST_ANALOG_BLACK, 0 },
    { "lt",    FAKE_ANALOG, HOST_ANALOG_LTRIG, 0 }, { "rt",    FAKE_ANALOG, HOST_ANALOG_RTRIG, 0 },
    { "lthumb", FAKE_BUTTON, XBOX_LTHUMB, 0 },    { "rthumb", FAKE_BUTTON, XBOX_RTHUMB, 0 },
    { "lstick_up",    FAKE_STICK, STICK_LY,  1 }, { "lstick_down",  FAKE_STICK, STICK_LY, -1 },
    { "lstick_left",  FAKE_STICK, STICK_LX, -1 }, { "lstick_right", FAKE_STICK, STICK_LX,  1 },
    { "rstick_up",    FAKE_STICK, STICK_RY,  1 }, { "rstick_down",  FAKE_STICK, STICK_RY, -1 },
    { "rstick_left",  FAKE_STICK, STICK_RX, -1 }, { "rstick_right", FAKE_STICK, STICK_RX,  1 },
};

/* Name -> FAKE index, or -1. `n` is the name's length inside a longer spec. */
static int fake_lookup(const char *p, size_t n)
{
    int i;
    for (i = 0; i < (int)(sizeof FAKE / sizeof FAKE[0]); i++)
        if (strlen(FAKE[i].name) == n && strncmp(p, FAKE[i].name, n) == 0)
            return i;
    return -1;
}

static void fake_apply(RecompInputGamepad *g, int i)
{
    int16_t push = (int16_t)(FAKE[i].sign > 0 ? 32767 : -32767);

    switch (FAKE[i].kind) {
    case FAKE_ANALOG:
        g->analog_buttons[FAKE[i].value] = 0xFFu;
        break;
    case FAKE_STICK:
        switch (FAKE[i].value) {
        case STICK_LX: g->thumb_lx = push; break;
        case STICK_LY: g->thumb_ly = push; break;
        case STICK_RX: g->thumb_rx = push; break;
        default:       g->thumb_ry = push; break;
        }
        break;
    default:
        g->buttons |= (uint16_t)FAKE[i].value;
        break;
    }
}

/* RECOMP_INPUT_SEQ=1500:start,4000:a,6000:down+a:400 -- a one-shot script.
 *
 * Each step is <ms>:<control>[+<control>...][:<hold ms>]. A control is a
 * button, an analog button, or a stick direction held all the way over
 * (lstick_up, rstick_left, ...), so a script can walk and turn, not only
 * press. The clock starts the
 * first time the title reads the pad, so a slow boot does not eat the script;
 * hold defaults to 200 ms, long enough for a title polling at 60 Hz to see the
 * press and the release. Steps may overlap, which is how a chord is held.
 *
 * This exists because RECOMP_FAKE_INPUT cycles forever: it is right for
 * "get past Press START" and wrong for a menu, where it keeps pressing things
 * at whatever screen it reached. A menu path is a fixed sequence, and there is
 * no save state to restore a native process to -- lifted code is mid-flight
 * on real threads with host GPU objects behind it -- so replaying the presses
 * is how a run gets back to the same screen. The sequence is the save state.
 * Nothing is pressed after the last step ends.
 *
 * There is no step limit. SEQ_MAX was 128, and a longer script was cut short
 * in silence: a six-minute MK Shaolin Monks play test stopped pressing at
 * three minutes and looked like the game had stopped answering the pad. At
 * 1024 a recorded play session (RECOMP_INPUT_RECORD below) still did: six and
 * a half minutes of TimeSplitters 2 is 1250 steps. The array grows instead.
 *
 * RECOMP_INPUT_SEQ_FILE=<path> reads the script from a file (the format
 * RECOMP_INPUT_RECORD writes), for a script too long to want in the
 * environment; RECOMP_INPUT_SEQ wins when both are set. */
struct seq_step { unsigned long at, hold; int keys[4]; int nkeys; };
static struct seq_step *s_seq;
static int s_seq_cap;
static int s_seq_count = -1;
static unsigned long long s_seq_t0;
static int s_seq_started;

/* RECOMP_INPUT_CLOCK=frames -- what a script's milliseconds are counted on.
 *
 * By default (wall) they are the host's wall clock, which is right only while
 * the title keeps up. Anything that slows it -- a debug switch, a check that
 * runs beside it, a loaded machine -- lets the script run ahead: a press meant
 * for the third menu lands on the second, and every step after it is on the
 * wrong screen. A 200 ms press can also fall wholly inside one slow frame and
 * never be seen.
 *
 * With frames, the clock is the frames the title has presented (its own Swap
 * or Present, counted at the HLE boundary: xbox_FpsSwapCount), and a step's
 * time t means "after t / 16.667 frames", so a script written in milliseconds
 * at 60 fps keeps its meaning however slowly the title runs. Presents, not
 * vblanks: the vblank pump is timed by the host and keeps delivering while
 * the title is stalled (a TimeSplitters 2 stretch slowed to 11 fps by a check
 * still got 25 vblanks a second), so counting vblanks drifts the way the wall
 * clock does, only less; a present is a frame the title finished. Frame
 * interpolation's extra frames are host presents and are not counted. What
 * this cannot see: a title that runs uncapped above 60 fps (the script runs
 * early, by the same ratio), and one whose logic steps by elapsed vblanks
 * rather than by frame (when slow it does more per frame, so the script runs
 * late; late is the safe side for a menu, which waits). While the title
 * presents nothing, as in a load that blocks, the script waits with it.
 *
 * RECOMP_INPUT_RECORD writes times on the same clock, so a session recorded
 * with frames replays on frames to the frame. RECOMP_FAKE_INPUT stays on the
 * wall clock: it presses forever and has no screens to stay in step with. */
static int input_clock_frames(void)
{
    static int frames = -1;

    if (frames < 0) {
        const char *v = getenv("RECOMP_INPUT_CLOCK");

        frames = 0;
        if (v && strcmp(v, "frames") == 0)
            frames = 1;
        else if (v && *v && strcmp(v, "wall") != 0)
            fprintf(stderr, "  [PAD] RECOMP_INPUT_CLOCK=%s: not 'frames' or 'wall';"
                    " using the wall clock\n", v);
        if (frames)
            fprintf(stderr, "  [PAD] input clock: presented frames, 16.667 ms a frame"
                    " (RECOMP_INPUT_CLOCK=frames)\n");
        fflush(stderr);
    }
    return frames;
}

/* Now, in milliseconds, on the clock RECOMP_INPUT_CLOCK chose. Frames are
 * converted as frames * 1000 / 60, which is monotonic and exact for a
 * recording replayed on the same clock: the frame a press was recorded on is
 * the first one whose time reaches it. */
static unsigned long long input_clock_ms(void)
{
    if (input_clock_frames()) {
        long long f = xbox_FpsSwapCount();
        return f > 0 ? (unsigned long long)f * 1000ull / 60ull : 0ull;
    }
    return GetTickCount64();
}

/* Under frames, a title whose frames never reach the HLE (no replaced Swap
 * or Present) would leave the script waiting in silence. Say so once, ten
 * seconds of wall clock after the clock started without a frame. */
static void input_clock_check(unsigned long long now, unsigned long long t0,
                              unsigned long long wall0)
{
    static int warned;

    if (warned || !input_clock_frames() || now != t0)
        return;
    if (GetTickCount64() - wall0 >= 10000ull) {
        warned = 1;
        fprintf(stderr, "  [PAD] RECOMP_INPUT_CLOCK=frames: no frame presented in 10 s since"
                " the first pad read, so the input clock has not moved (does the title's"
                " Swap or Present reach the HLE?)\n");
        fflush(stderr);
    }
}

static void seq_parse(const char *env)
{
    const char *p = env;

    while (*p) {
        struct seq_step *s;
        char *end;
        const char *step_end = strchr(p, ',');
        size_t len = step_end ? (size_t)(step_end - p) : strlen(p);
        const char *q;

        if (s_seq_count == s_seq_cap) {
            int cap = s_seq_cap ? s_seq_cap * 2 : 256;
            struct seq_step *grown = (struct seq_step *)realloc(s_seq, (size_t)cap * sizeof *grown);
            if (!grown) {
                fprintf(stderr, "  [PAD] RECOMP_INPUT_SEQ: out of memory after %d steps;"
                        " the rest are ignored\n", s_seq_count);
                return;
            }
            s_seq = grown;
            s_seq_cap = cap;
        }
        s = &s_seq[s_seq_count];
        memset(s, 0, sizeof *s);
        s->hold = 200;
        s->at = strtoul(p, &end, 10);
        if (end == p || *end != ':') {
            fprintf(stderr, "  [PAD] RECOMP_INPUT_SEQ: bad step at '%.*s' (want <ms>:<button>)\n",
                    (int)len, p);
            return;
        }
        q = end + 1;
        while (q < p + len) {
            size_t n = 0;
            int k;
            while (q + n < p + len && q[n] != '+' && q[n] != ':')
                n++;
            k = fake_lookup(q, n);
            if (k < 0) {
                fprintf(stderr, "  [PAD] RECOMP_INPUT_SEQ: unknown button '%.*s'\n", (int)n, q);
                return;
            }
            if (s->nkeys < 4)
                s->keys[s->nkeys++] = k;
            q += n;
            if (q < p + len && *q == ':') {
                s->hold = strtoul(q + 1, NULL, 10);
                if (!s->hold)
                    s->hold = 200;
                break;
            }
            if (q < p + len && *q == '+')
                q++;
        }
        s_seq_count++;
        if (!step_end) {
            p += len;
            break;
        }
        p = step_end + 1;
    }
}

/* The whole of a script file, line breaks and spaces dropped (a recording is
 * one line, but an edited one need not be); NULL if it cannot be read. */
static char *seq_read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *text = NULL;
    size_t len = 0, cap = 0;
    int c;

    if (!f)
        return NULL;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
            continue;
        if (len + 1 >= cap) {
            size_t ncap = cap ? cap * 2 : 4096;
            char *grown = (char *)realloc(text, ncap);
            if (!grown) {
                free(text);
                fclose(f);
                return NULL;
            }
            text = grown;
            cap = ncap;
        }
        text[len++] = (char)c;
    }
    fclose(f);
    if (text)
        text[len] = 0;
    return text;
}

/* Whether an input script is driving this run (RECOMP_INPUT_SEQ), parsing
 * it the first time. */
static int seq_active(void)
{
    if (s_seq_count < 0) {
        const char *env = getenv("RECOMP_INPUT_SEQ");
        const char *path = getenv("RECOMP_INPUT_SEQ_FILE");
        char *file = NULL;
        s_seq_count = 0;
        if ((!env || !*env) && path && *path) {
            file = seq_read_file(path);
            if (!file)
                fprintf(stderr, "  [PAD] RECOMP_INPUT_SEQ_FILE=%s: cannot read it\n", path);
            env = file;
        }
        if (env && *env) {
            seq_parse(env);
            /* The start, not the whole: a recorded session is tens of KB. */
            fprintf(stderr, "  [PAD] input script: %d steps, from %s, starting %.80s%s\n",
                    s_seq_count, file ? path : "RECOMP_INPUT_SEQ", env,
                    strlen(env) > 80 ? "..." : "");
            fflush(stderr);
        }
        free(file);
    }
    return s_seq_count > 0;
}

/* A script is for a run that goes the same way every time, and a pad left
 * connected on the desk does not: a resting stick's drift walked
 * TimeSplitters 2's name-entry cursor off END, so the script's A typed
 * letters instead of finishing and the run never left the menus. While a
 * script runs it is the only input on port 0, unless
 * RECOMP_INPUT_SEQ_PAD=1 asks for the pad as well. */
static int seq_owns_pad(void)
{
    static int with_pad = -1;

    if (!seq_active())
        return 0;
    if (with_pad < 0) {
        const char *v = getenv("RECOMP_INPUT_SEQ_PAD");

        with_pad = v && *v && strcmp(v, "0") != 0;
        if (!with_pad)
            fprintf(stderr, "  [PAD] the input script replaces the pad on port 0 "
                    "(RECOMP_INPUT_SEQ_PAD=1 to have both)\n");
    }
    return !with_pad;
}

static void seq_input(RecompInputGamepad *g)
{
    static unsigned long long wall0;
    unsigned long long now, t;
    int i, k;

    if (!seq_active())
        return;
    now = input_clock_ms();
    if (!s_seq_started) {
        s_seq_started = 1;
        s_seq_t0 = now;
        wall0 = GetTickCount64();
    }
    input_clock_check(now, s_seq_t0, wall0);
    t = now - s_seq_t0;
    for (i = 0; i < s_seq_count; i++) {
        if (t < s_seq[i].at || t >= (unsigned long long)s_seq[i].at + s_seq[i].hold)
            continue;
        for (k = 0; k < s_seq[i].nkeys; k++)
            fake_apply(g, s_seq[i].keys[k]);
    }
}

static void fake_input(RecompInputGamepad *g)
{
    static int checked;
    static char spec[64];
    static unsigned long period;
    const char *p;
    unsigned long long now;
    int count = 1, want, index = 0, i;

    if (!checked) {
        const char *env = getenv("RECOMP_FAKE_INPUT");
        const char *ms = getenv("RECOMP_FAKE_INPUT_MS");
        checked = 1;
        if (env) {
            strncpy(spec, env, sizeof spec - 1);
            fprintf(stderr, "  [PAD] synthetic input: %s\n", spec);
            fflush(stderr);
        }
        period = ms ? strtoul(ms, NULL, 0) : 0;
        if (!period)
            period = 500;
    }
    if (!spec[0])
        return;
    now = GetTickCount64();
    if ((now / period) % 2 == 0)
        return;                                 /* the gap half of the cycle */
    for (p = spec; *p; p++)
        if (*p == ',')
            count++;
    want = (int)((now / (period * 2)) % (unsigned long long)count);
    p = spec;
    while (index < want && (p = strchr(p, ',')) != NULL) {
        p++;
        index++;
    }
    if (!p)
        return;
    {
        const char *e = strchr(p, ',');
        i = fake_lookup(p, e ? (size_t)(e - p) : strlen(p));
        if (i >= 0)
            fake_apply(g, i);
    }
}

/* RECOMP_INPUT_RECORD=<file> -- the other half of RECOMP_INPUT_SEQ: what a
 * person presses on controller 1, written as a script that replays it.
 *
 * A menu path or a walk to a place in a level is quicker to play once than to
 * write by trial, and a script played by hand is one that is known to work.
 * The clock starts at the title's first pad read, as the replay's does, so the
 * same file played back presses at the same moments; under
 * RECOMP_INPUT_CLOCK=frames the times are presented frames, as milliseconds
 * at 60 fps, and the file should be replayed on that clock too. Each press is written
 * when it is released, as <ms>:<control>:<hold ms>, comma-separated on one
 * line, so the file is the value RECOMP_INPUT_SEQ wants as it stands. Sticks
 * are recorded as pushed fully one way past half travel, which is all the
 * script can replay; a press shorter than 120 ms is kept at 120 ms so a title
 * polling at 30 Hz still sees it on replay. Recording ignores a script that is
 * also running, so a recorded run can start from a scripted menu path. */
#define RECORD_MIN_HOLD 120u
static FILE *s_rec;
static int s_rec_checked, s_rec_started;
static unsigned long long s_rec_t0, s_rec_down[sizeof FAKE / sizeof FAKE[0]];
static unsigned char s_rec_on[sizeof FAKE / sizeof FAKE[0]];
static unsigned long s_rec_steps;

static int record_pressed(const RecompInputGamepad *g, int i)
{
    int v;

    switch (FAKE[i].kind) {
    case FAKE_ANALOG:
        return g->analog_buttons[FAKE[i].value] > 32u;
    case FAKE_STICK:
        switch (FAKE[i].value) {
        case STICK_LX: v = g->thumb_lx; break;
        case STICK_LY: v = g->thumb_ly; break;
        case STICK_RX: v = g->thumb_rx; break;
        default:       v = g->thumb_ry; break;
        }
        return FAKE[i].sign > 0 ? v > 16384 : v < -16384;
    default:
        return (g->buttons & FAKE[i].value) != 0;
    }
}

static void record_input(const RecompInputGamepad *g)
{
    unsigned long long t;
    int i;

    if (!s_rec_checked) {
        const char *path = getenv("RECOMP_INPUT_RECORD");
        s_rec_checked = 1;
        if (path && *path) {
            s_rec = fopen(path, "w");
            fprintf(stderr, "  [PAD] recording controller 1 to %s%s\n", path,
                    s_rec ? "" : " -- could not open it");
            fflush(stderr);
        }
    }
    if (!s_rec)
        return;
    t = input_clock_ms();
    if (!s_rec_started) {
        s_rec_started = 1;
        s_rec_t0 = t;
    }
    t -= s_rec_t0;
    for (i = 0; i < (int)(sizeof FAKE / sizeof FAKE[0]); i++) {
        int on;

        if (strncmp(FAKE[i].name, "dpad_", 5) == 0)
            continue;                       /* aliases of up/down/left/right */
        on = record_pressed(g, i);
        if (on && !s_rec_on[i]) {
            s_rec_down[i] = t;
        } else if (!on && s_rec_on[i]) {
            unsigned long long hold = t - s_rec_down[i];
            if (hold < RECORD_MIN_HOLD)
                hold = RECORD_MIN_HOLD;
            fprintf(s_rec, "%s%llu:%s:%llu", s_rec_steps++ ? "," : "",
                    s_rec_down[i], FAKE[i].name, hold);
            fflush(s_rec);
        }
        s_rec_on[i] = (unsigned char)on;
    }
}

/* One port, through its bindings. The bound sample arrives as the layout
 * src/input already speaks (XBOX_GAMEPAD); the two structures are the same
 * eight analog bytes and four axes in the same order, so this is a copy. */
bool recomp_input_host_sample_port(unsigned port, RecompInputGamepad *gamepad)
{
    XBOX_GAMEPAD pad;
    int i;

    if (gamepad == NULL)
        return false;
    memset(gamepad, 0, sizeof *gamepad);
    if (!(port == 0 && seq_owns_pad()) && recomp_bindings_sample(port, &pad)) {
        gamepad->buttons = (uint16_t)pad.wButtons;
        for (i = 0; i < RECOMP_INPUT_ANALOG_BUTTON_COUNT; i++)
            gamepad->analog_buttons[i] = pad.bAnalogButtons[i];
        gamepad->thumb_lx = (int16_t)pad.sThumbLX;
        gamepad->thumb_ly = (int16_t)pad.sThumbLY;
        gamepad->thumb_rx = (int16_t)pad.sThumbRX;
        gamepad->thumb_ry = (int16_t)pad.sThumbRY;
    }
    if (port == 0) {
        record_input(gamepad);      /* the person, before any script adds to it */
        fake_input(gamepad);
        seq_input(gamepad);
    }
    return true;
}

bool recomp_input_host_sample(RecompInputGamepad *gamepad)
{
    return recomp_input_host_sample_port(0u, gamepad);
}
