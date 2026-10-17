/*
 * recomp_config.c -- see recomp_config.h for what this is and why.
 *
 * The parser is deliberately small. Blank lines and lines starting with
 * '#' or ';' are comments, everything else is `key = value` with the
 * spaces optional, and a key it does not recognise is ignored rather than
 * refused -- a config written by a newer launcher must not stop an older
 * build from running.
 */
#include "recomp_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>   /* GetModuleFileNameA: the portable folder */
#include <direct.h>
#include <sys/types.h>
#include <sys/stat.h>
#define RECOMP_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define RECOMP_MKDIR(p) mkdir((p), 0755)
#endif

#define MAX_ENTRIES 64
#define MAX_KEY     64
#define MAX_VALUE   512

/* The NV2A's vertex constant registers, c[0..191]: NV2A_VS_MAX_CONSTANTS in
 * src/kernel/nv2a_vsh.h, which this library cannot include -- it depends
 * on nothing, so that everything can depend on it. */
#define VS_CONSTANTS 192

typedef struct {
    char key[MAX_KEY];
    char value[MAX_VALUE];
} Entry;

static Entry   g_entries[MAX_ENTRIES];
static int     g_entry_count;
static int     g_loaded;
static uint32_t g_title_id;
static char    g_path[1024];
static int     g_have_path;

void recomp_config_set_title(uint32_t title_id)
{
    g_title_id = title_id;
}

uint32_t recomp_config_title_id(void)
{
    return g_title_id;
}

/* Defined below, beside the lookups it belongs to; needed by the
 * settings reader above it. */
static const char *from_table(const char *key);

/* ----------------------------------------------------------------- text */

/* Case-insensitive compare, spelled out rather than borrowed: stricmp is
 * MSVC's and strcasecmp is POSIX's, and this file is built for both. */
static int ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++; b++;
    }
    return *a == *b;
}

/* The words that mean off, everywhere in the runtime. Anything else,
 * including an empty string, means on. */
static int off_word(const char *v)
{
    return strcmp(v, "0") == 0 || ieq(v, "off") || ieq(v, "no") || ieq(v, "false");
}

static char *trim(char *s)
{
    char *end;

    while (*s && isspace((unsigned char)*s))
        s++;
    if (!*s)
        return s;
    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        *end-- = '\0';
    return s;
}

static void store(const char *key, const char *value)
{
    int i;

    for (i = 0; i < g_entry_count; i++)
        if (strcmp(g_entries[i].key, key) == 0) {
            snprintf(g_entries[i].value, sizeof g_entries[i].value, "%s", value);
            return;
        }
    if (g_entry_count >= MAX_ENTRIES)
        return;
    snprintf(g_entries[g_entry_count].key, MAX_KEY, "%s", key);
    snprintf(g_entries[g_entry_count].value, MAX_VALUE, "%s", value);
    g_entry_count++;
}

static void parse(FILE *f)
{
    char line[MAX_VALUE + MAX_KEY + 8];

    while (fgets(line, sizeof line, f)) {
        char *p = trim(line), *eq;

        if (!*p || *p == '#' || *p == ';')
            continue;
        eq = strchr(p, '=');
        if (!eq)
            continue;                    /* not a setting; say nothing */
        *eq = '\0';
        store(trim(p), trim(eq + 1));
    }
}

/* ----------------------------------------------------------------- paths */

/* Per-user, beside the input bindings, because a setting is a judgement
 * about this machine rather than about this copy of the game. */
/* <base>/xboxrecomp, where base is the environment variable `var` if it is
 * set, else $HOME/<home_rel> (POSIX). 0 if neither gives a base. */
static int dir_from(char *out, size_t n, const char *var, const char *home_rel)
{
    const char *base = var ? getenv(var) : NULL;
    int len;

#if defined(_WIN32)
    (void)home_rel;
    if (!base || !*base)
        return 0;
    len = snprintf(out, n, "%s\\xboxrecomp", base);
#else
    if (base && *base) {
        len = snprintf(out, n, "%s/xboxrecomp", base);
    } else {
        const char *h = getenv("HOME");
        if (!h || !*h || !home_rel)
            return 0;
        len = snprintf(out, n, "%s/%s/xboxrecomp", h, home_rel);
    }
#endif
    return len > 0 && (size_t)len < n;
}

/* RECOMP_USER_DIR=<dir> stands in for all three: settings and saves in it,
 * caches under it. For tests and scripted runs, which must not read or
 * write the player's own (RECOMP_SAVE_DIR still decides the saves).
 *
 * 1 when it gave the directory, 0 when it is unset, -1 when it is set but
 * does not fit. The last must not fall through to the player's directory --
 * that is the one thing the switch exists to prevent -- so the helpers
 * report no directory at all instead. */
static int override_dir(char *out, size_t n, const char *tail)
{
    const char *v = getenv("RECOMP_USER_DIR");
    int len;
    if (!v || !*v)
        return 0;
    len = snprintf(out, n, "%s%s", v, tail);
    if (len > 0 && (size_t)len < n)
        return 1;
    if (n)
        out[0] = '\0';
    return -1;
}

static int user_dir(char *out, size_t n)
{
    int o;
    if ((o = override_dir(out, n, "")) != 0)
        return o > 0;
#if defined(_WIN32)
    /* Portable: an "xboxrecomp" folder beside the executable holds the
     * settings and bindings instead of %APPDATA%. Also the way around
     * AppData being redirected per app (MSIX packages: a tool started
     * from one writes a private copy the player's own launch never sees). */
    {
        DWORD len = GetModuleFileNameA(NULL, out, (DWORD)n);
        char *slash;
        if (len && len < n && (slash = strrchr(out, '\\')) != NULL &&
            (size_t)(slash - out) + sizeof "\\xboxrecomp" <= n) {
            DWORD a;
            strcpy(slash, "\\xboxrecomp");
            a = GetFileAttributesA(out);
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
                return 1;
        }
    }
    return dir_from(out, n, "APPDATA", NULL);
#elif defined(__APPLE__)
    return dir_from(out, n, NULL, "Library/Application Support");
#else
    return dir_from(out, n, "XDG_CONFIG_HOME", ".config");
#endif
}

int recomp_config_user_dir(char *out, size_t n)
{
    return user_dir(out, n);
}

int recomp_data_dir(char *out, size_t n)
{
    int o;
    if ((o = override_dir(out, n, "")) != 0)
        return o > 0;
#if defined(_WIN32)
    return dir_from(out, n, "LOCALAPPDATA", NULL);
#elif defined(__APPLE__)
    return dir_from(out, n, NULL, "Library/Application Support");
#else
    return dir_from(out, n, "XDG_DATA_HOME", ".local/share");
#endif
}

int recomp_cache_dir(char *out, size_t n)
{
#if defined(_WIN32)
    int o = override_dir(out, n, "\\cache");
#else
    int o = override_dir(out, n, "/cache");
#endif
    if (o != 0)
        return o > 0;
#if defined(_WIN32)
    return user_dir(out, n);
#elif defined(__APPLE__)
    return dir_from(out, n, NULL, "Library/Caches");
#else
    return dir_from(out, n, "XDG_CACHE_HOME", ".cache");
#endif
}

int recomp_make_dirs(const char *path)
{
    char buf[1024];
    size_t len = path ? strlen(path) : 0;
    size_t i;

    if (!len || len >= sizeof buf)
        return 0;
    memcpy(buf, path, len + 1);
    /* Each prefix ending at a separator, then the whole path. A drive root
     * ("C:\") or "/" fails to mkdir harmlessly: it exists. */
    for (i = 1; i < len; i++) {
        if (buf[i] == '/' || buf[i] == '\\') {
            char c = buf[i];
            buf[i] = '\0';
            RECOMP_MKDIR(buf);
            buf[i] = c;
        }
    }
    RECOMP_MKDIR(buf);
    {
#if defined(_WIN32)
        struct _stat st;
        return _stat(buf, &st) == 0 && (st.st_mode & _S_IFDIR);
#else
        struct stat st;
        return stat(buf, &st) == 0 && S_ISDIR(st.st_mode);
#endif
    }
}

static void title_file(char *out, size_t n, const char *dir)
{
#if defined(_WIN32)
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    if (g_title_id)
        snprintf(out, n, "%s%ctitles%c%08X.conf", dir, sep, sep, g_title_id);
    else
        snprintf(out, n, "%s%ctitles%cdefault.conf", dir, sep, sep);
}

static int readable(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (!f)
        return 0;
    fclose(f);
    return 1;
}

/* ------------------------------------------------------------ settings */

void recomp_settings_defaults(RecompSettings *s)
{
    if (!s)
        return;
    memset(s, 0, sizeof *s);
    s->resolution_scale  = 1;
    s->widescreen        = 0;
    s->hor_plus          = RECOMP_HOR_PLUS_AUTO;
    s->hor_plus_register = RECOMP_REGISTER_AUTO;
    snprintf(s->widescreen_2d, sizeof s->widescreen_2d, "auto");
    s->anisotropy        = 1;
    s->fps_overlay       = 0;
    s->fullscreen        = 0;
    s->vrr               = 1;
    s->frame_interp      = 1;
    snprintf(s->frame_cap, sizeof s->frame_cap, "adaptive");
}

static int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

int recomp_settings_read(const char *path, RecompSettings *s)
{
    Entry saved[MAX_ENTRIES];
    int saved_count = g_entry_count, saved_loaded = g_loaded;
    FILE *f;

    if (!path || !s)
        return 0;
    recomp_settings_defaults(s);

    f = fopen(path, "rb");
    if (!f)
        return 0;

    /* The table is the lookup path's; borrow it, then put it back, so a
     * launcher reading a file does not disturb a runtime that has
     * already loaded its own. */
    memcpy(saved, g_entries, sizeof saved);
    g_entry_count = 0;
    parse(f);
    fclose(f);

    {
        const char *v;

        if ((v = from_table("resolution_scale")) != NULL)
            s->resolution_scale = clamp_int(atoi(v), 1, 8);
        if ((v = from_table("widescreen")) != NULL)
            s->widescreen = !off_word(v);
        /* A number is an override and is kept as one; "auto", like no
         * line at all, leaves it to the title (recomp_widescreen_resolve).
         * Nothing below zero is a factor, and the runtime has always read
         * one as "leave the camera alone", so it is kept as that rather
         * than colliding with the mark for auto. */
        if ((v = from_table("hor_plus")) != NULL) {
            double factor = atof(v);

            if (ieq(v, "auto"))
                s->hor_plus = RECOMP_HOR_PLUS_AUTO;
            else
                s->hor_plus = factor < 0.0 ? 0.0 : factor;
        }
        if ((v = from_table("hor_plus_register")) != NULL) {
            if (ieq(v, "auto"))
                s->hor_plus_register = RECOMP_REGISTER_AUTO;
            else
                s->hor_plus_register = clamp_int(atoi(v), 0, VS_CONSTANTS - 1);
        }
        if ((v = from_table("widescreen_2d")) != NULL)
            snprintf(s->widescreen_2d, sizeof s->widescreen_2d, "%s", v);
        if ((v = from_table("anisotropy")) != NULL)
            s->anisotropy = clamp_int(atoi(v), 1, 16);
        if ((v = from_table("frame_cap")) != NULL)
            snprintf(s->frame_cap, sizeof s->frame_cap, "%s", v);
        if ((v = from_table("fps_overlay")) != NULL)
            s->fps_overlay = !off_word(v);
        if ((v = from_table("fullscreen")) != NULL)
            s->fullscreen = !off_word(v);
        if ((v = from_table("vrr")) != NULL)
            s->vrr = !off_word(v);
        if ((v = from_table("frame_interp")) != NULL)
            s->frame_interp = clamp_int(atoi(v), 1, 4);
        if ((v = from_table("game_dir")) != NULL)
            snprintf(s->game_dir, sizeof s->game_dir, "%s", v);
    }

    memcpy(g_entries, saved, sizeof saved);
    g_entry_count = saved_count;
    g_loaded = saved_loaded;
    return 1;
}

/* Everything above the file's directory, made if it is not there. */
static void make_parents(const char *path)
{
    char buf[1024];
    size_t i;

    snprintf(buf, sizeof buf, "%s", path);
    for (i = 0; buf[i]; i++) {
        if (buf[i] == '\\' || buf[i] == '/') {
            char c = buf[i];

            buf[i] = '\0';
            if (i > 0)
                RECOMP_MKDIR(buf);
            buf[i] = c;
        }
    }
}

/* The one place the file's text is written: the runtime's first-run
 * default and the launcher's save are the same function, so the comments
 * a player reads cannot drift from the keys the runtime looks for. */
int recomp_settings_write(const char *path, const RecompSettings *s)
{
    RecompSettings d;
    char hor_plus[48], reg[48];
    FILE *f;

    if (!path)
        return 0;
    if (!s) {
        recomp_settings_defaults(&d);
        s = &d;
    }

    make_parents(path);
    f = fopen(path, "wb");
    if (!f)
        return 0;

    /* An override is written as a line, auto as the same line commented
     * out. Not as the word alone: a build from before "auto" would read it
     * as a number -- 0, which for the register is c[0] -- where a missing
     * key is something every build has always understood. */
    if (s->hor_plus < 0.0)
        snprintf(hor_plus, sizeof hor_plus, "# hor_plus = auto");
    else
        snprintf(hor_plus, sizeof hor_plus, "hor_plus = %g", s->hor_plus);
    if (s->hor_plus_register < 0)
        snprintf(reg, sizeof reg, "# hor_plus_register = auto");
    else
        snprintf(reg, sizeof reg, "hor_plus_register = %d", s->hor_plus_register);

    fprintf(f,
        "# Settings for this title, written by the launcher and read when the\n"
        "# game starts. Every line can be overridden by the environment\n"
        "# variable named beside it, so a .bat or a command line still wins.\n"
        "#\n"
        "# Delete this file to go back to the defaults.\n"
        "\n"
        "# How much larger than the console the game is rendered, before being\n"
        "# filtered back down: supersampling. 1 is the console's own size. 2 is\n"
        "# a good default and costs little. Up to 8.        [RECOMP_RES_SCALE]\n"
        "resolution_scale = %d\n"
        "\n"
        "# Widescreen: play at 16:9 rather than 4:3. The game is told the\n"
        "# console is widescreen and its picture is shown at 16:9; what more\n"
        "# that takes is worked out for the game when it starts. One with a\n"
        "# 16:9 mode of its own needs nothing else. One without has its camera\n"
        "# widened to match, so you see more to the sides rather than the same\n"
        "# view stretched -- once its project has said it is that kind. A game\n"
        "# nobody has described is treated as the first kind and, if it is\n"
        "# not, looks stretched.                           [RECOMP_WIDESCREEN]\n"
        "widescreen = %d\n"
        "\n"
        "# The next two follow from the line above and are here to be\n"
        "# overridden, not chosen: take the '#' off a line to set it yourself.\n"
        "#\n"
        "# How far the camera is widened, as the projection's horizontal\n"
        "# scale. 0.75 is the 4:3-to-16:9 figure; 0 leaves the camera alone.\n"
        "# A number here applies whatever the game, widescreen or not. auto,\n"
        "# or no line, widens only a game that needs it, and only in\n"
        "# widescreen.                                       [RECOMP_HOR_PLUS]\n"
        "%s\n"
        "#\n"
        "# Which vertex constant register holds the game's projection. auto,\n"
        "# or no line, is the one the game's project names, else 60.\n"
        "#                                               [RECOMP_HOR_PLUS_REG]\n"
        "%s\n"
        "\n"
        "# In widescreen, where the game's 2D layer (menus, HUD, text) goes.\n"
        "# auto keeps it at 4:3 in the middle but lets anything most of the\n"
        "# screen wide span it. centre keeps all of it at 4:3, backdrops too,\n"
        "# and lets only whole-screen fades and effects span: no seams where\n"
        "# a game's menu layers meet, at the price of bars beside 2D-only\n"
        "# screens.                                      [RECOMP_WIDESCREEN_2D]\n"
        "widescreen_2d = %s\n"
        "\n"
        "# Sharpen textures seen at a glancing angle, 1 to 16. The flat layer\n"
        "# is left alone, which wants no filtering.              [RECOMP_ANISO]\n"
        "anisotropy = %d\n"
        "\n"
        "# Frame pacing: adaptive, 60, 30, or 0 for uncapped.  [RECOMP_FPS_CAP]\n"
        "frame_cap = %s\n"
        "\n"
        "# Show the frame rate from the moment the game starts. F9 toggles it\n"
        "# while playing either way.                      [RECOMP_FPS_OVERLAY]\n"
        "fps_overlay = %d\n"
        "\n"
        "# Fill the screen the window is on, without borders. Alt+Enter\n"
        "# switches while playing either way.             [RECOMP_FULLSCREEN]\n"
        "fullscreen = %d\n"
        "\n"
        "# Variable refresh rate (G-SYNC, FreeSync): in fullscreen, hand each\n"
        "# frame to the display the moment it is ready, so a 60 fps game is\n"
        "# shown at an even 60 on a 144 Hz screen. The display and its driver\n"
        "# must have it switched on. 0 turns it off.             [RECOMP_VRR]\n"
        "vrr = %d\n"
        "\n"
        "# Frame interpolation: draw frames between the game's own, so 2 shows\n"
        "# 120 a second while the game itself still runs at 60. Best with\n"
        "# variable refresh. 1 is off.                   [RECOMP_FRAME_INTERP]\n"
        "frame_interp = %d\n"
        "\n"
        "# Where the game's files are, if they are not beside the executable.\n"
        "#                                                   [RECOMP_GAME_DIR]\n",
        s->resolution_scale, s->widescreen, hor_plus, reg,
        s->widescreen_2d[0] ? s->widescreen_2d : "auto",
        s->anisotropy, s->frame_cap[0] ? s->frame_cap : "adaptive", s->fps_overlay,
        s->fullscreen, s->vrr, s->frame_interp < 1 ? 1 : s->frame_interp);

    if (s->game_dir[0])
        fprintf(f, "game_dir = %s\n", s->game_dir);
    else
        fprintf(f, "# game_dir =\n");

    fclose(f);
    return 1;
}

void recomp_settings_set_widescreen(RecompSettings *s, int on)
{
    if (!s)
        return;
    on = on ? 1 : 0;
    if (s->widescreen != on) {
        s->widescreen = on;
        s->hor_plus = RECOMP_HOR_PLUS_AUTO;
    }
    if (s->hor_plus_register == RECOMP_PROJECTION_REGISTER_DEFAULT)
        s->hor_plus_register = RECOMP_REGISTER_AUTO;
}

int recomp_settings_path(uint32_t title_id, char *out, size_t n)
{
    const char *env = getenv("RECOMP_DISPLAY_CONFIG");
    char dir[1024];

    if (!out || !n)
        return 0;
    if (env && *env) {
        snprintf(out, n, "%s", env);
        return 1;
    }
    if (!user_dir(dir, sizeof dir))
        return 0;
    {
        uint32_t saved = g_title_id;

        g_title_id = title_id;
        title_file(out, n, dir);
        g_title_id = saved;
    }
    return 1;
}

static void load(void)
{
    const char *env = getenv("RECOMP_DISPLAY_CONFIG");
    char dir[1024], path[1024];
    FILE *f;

    g_loaded = 1;

    /* Named outright: use it or say so, and do not quietly fall back --
     * a mistyped path should not look like the settings were ignored. */
    if (env && *env) {
        f = fopen(env, "rb");
        if (f) {
            parse(f);
            fclose(f);
            snprintf(g_path, sizeof g_path, "%s", env);
            g_have_path = 1;
        } else {
            fprintf(stderr, "[CONFIG] RECOMP_DISPLAY_CONFIG=%s cannot be read; "
                            "using the defaults\n", env);
        }
        return;
    }

    if (user_dir(dir, sizeof dir)) {
        title_file(path, sizeof path, dir);
        if (!readable(path))
            recomp_settings_write(path, NULL);
        f = fopen(path, "rb");
        if (f) {
            parse(f);
            fclose(f);
            snprintf(g_path, sizeof g_path, "%s", path);
            g_have_path = 1;
            return;
        }
    }

    /* Beside the executable, which is where a portable copy would keep
     * it -- the working directory a launcher starts the game in. */
    f = fopen("recomp.conf", "rb");
    if (f) {
        parse(f);
        fclose(f);
        snprintf(g_path, sizeof g_path, "recomp.conf");
        g_have_path = 1;
    }
}

/* Said once, because "which file did it read" is the first question when a
 * setting chosen in the launcher does not seem to apply. */
static void load_and_report(void)
{
    load();
    if (g_have_path)
        fprintf(stderr, "[CONFIG] settings from %s (title %08X)\n", g_path, g_title_id);
    else
        fprintf(stderr, "[CONFIG] no settings file (title %08X); using the defaults\n",
                g_title_id);
}

/* ---------------------------------------------------------------- lookup */

/* The table as it stands, without loading anything. */
static const char *from_table(const char *key)
{
    int i;

    if (!key)
        return NULL;
    for (i = 0; i < g_entry_count; i++)
        if (strcmp(g_entries[i].key, key) == 0)
            return g_entries[i].value[0] ? g_entries[i].value : NULL;
    return NULL;
}

static const char *from_file(const char *key)
{
    if (!key)
        return NULL;
    if (!g_loaded)
        load_and_report();
    return from_table(key);
}

const char *recomp_config_lookup(const char *env_name, const char *key)
{
    const char *v = env_name ? getenv(env_name) : NULL;

    if (v && *v)
        return v;
    return from_file(key);
}

const char *recomp_config_path(void)
{
    if (!g_loaded)
        load_and_report();
    return g_have_path ? g_path : NULL;
}

int recomp_config_int(const char *env_name, const char *key, int fallback)
{
    const char *v = recomp_config_lookup(env_name, key);
    char *end = NULL;
    long n;

    if (!v || !*v)
        return fallback;
    n = strtol(v, &end, 10);
    if (end == v || (end && *end))
        return fallback;
    return (int)n;
}

double recomp_config_float(const char *env_name, const char *key, double fallback)
{
    const char *v = recomp_config_lookup(env_name, key);
    char *end = NULL;
    double d;

    if (!v || !*v)
        return fallback;
    d = strtod(v, &end);
    if (end == v || (end && *end))
        return fallback;
    return d;
}

int recomp_config_bool(const char *env_name, const char *key, int fallback)
{
    const char *env = env_name ? getenv(env_name) : NULL;
    const char *v;

    /* An empty environment variable means on, as it does everywhere else
     * in the runtime: `set RECOMP_WIDESCREEN=` turns a switch on. */
    if (env)
        return !off_word(env);

    v = from_file(key);
    if (!v || !*v)
        return fallback;
    return !off_word(v);
}

/* ------------------------------------------------------------ widescreen */

/* What the title's project said in its CMakeLists
 * (recomp_title_widescreen, src/config/CMakeLists.txt), which arrives as
 * these two definitions on this file. Compiled in rather than read from a
 * file beside the game: there is nothing to go missing when an executable
 * is copied somewhere, and the launcher, which links this library too, is
 * told the same thing by the same line. */
#ifndef RECOMP_TITLE_WIDESCREEN
#define RECOMP_TITLE_WIDESCREEN 0           /* RECOMP_WIDE_UNKNOWN */
#endif
#ifndef RECOMP_TITLE_PROJECTION_REGISTER
#define RECOMP_TITLE_PROJECTION_REGISTER (-1)
#endif

static RecompWideMode g_wide_mode = (RecompWideMode)(RECOMP_TITLE_WIDESCREEN);
static int            g_wide_register = (RECOMP_TITLE_PROJECTION_REGISTER);
static unsigned       g_wide_serial;

void recomp_widescreen_title(RecompWideMode mode, int projection_register)
{
    g_wide_mode = mode;
    if (projection_register >= 0 && projection_register < VS_CONSTANTS)
        g_wide_register = projection_register;
    g_wide_serial++;
}

RecompWideMode recomp_widescreen_title_mode(void)
{
    return g_wide_mode;
}

unsigned recomp_widescreen_title_serial(void)
{
    return g_wide_serial;
}

void recomp_widescreen_resolve_from(int on, const char *hor_plus, const char *reg,
                                    RecompWideMode mode, int title_register,
                                    RecompWidescreen *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof *out);
    out->on = on ? 1 : 0;
    out->mode = mode;
    out->title_register =
        (title_register >= 0 && title_register < VS_CONSTANTS) ? title_register : -1;

    /* A number is somebody's decision and is taken as given, with
     * widescreen off as well: RECOMP_HOR_PLUS alone has always widened the
     * view, which is how a projection register is hunted for. 0, and
     * anything else that is not a usable factor, leaves the camera alone
     * -- for a HOR_PLUS title too, which is how to see one stretched. */
    out->hor_plus = 1.0;
    if (hor_plus && *hor_plus && !ieq(hor_plus, "auto")) {
        double f = atof(hor_plus);

        out->hor_plus_named = 1;
        if (f > 0.0 && f <= 4.0)
            out->hor_plus = f;
    } else if (out->on && mode == RECOMP_WIDE_HOR_PLUS) {
        out->hor_plus = RECOMP_HOR_PLUS_16_9;
    }

    out->projection_register = out->title_register >= 0
                             ? out->title_register : RECOMP_PROJECTION_REGISTER_DEFAULT;
    if (reg && *reg && !ieq(reg, "auto")) {
        char *end = NULL;
        long n = strtol(reg, &end, 10);

        if (end != reg && n >= 0 && n < VS_CONSTANTS) {
            out->projection_register = (int)n;
            out->register_named = 1;
        }
    }
}

void recomp_widescreen_resolve(RecompWidescreen *out)
{
    recomp_widescreen_resolve_from(
        recomp_config_bool("RECOMP_WIDESCREEN", "widescreen", 0),
        recomp_config_lookup("RECOMP_HOR_PLUS", "hor_plus"),
        recomp_config_lookup("RECOMP_HOR_PLUS_REG", "hor_plus_register"),
        g_wide_mode, g_wide_register, out);
}
