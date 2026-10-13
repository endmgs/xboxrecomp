/*
 * recomp_config.h -- the settings a player chooses, and where they live.
 *
 * Everything the runtime can be told is an environment variable, which is
 * right for a developer and no use to someone who just wants the game in
 * widescreen. This is the other half: a small file the launcher writes and
 * the runtime reads, so a setting survives being chosen once.
 *
 * The environment still wins. A variable set on the command line, in a
 * .bat, or by CI overrides the file, so every existing way of driving the
 * runtime keeps working and the file is what happens when nothing said
 * otherwise. Callers say both names at the point of use:
 *
 *     v = recomp_config_lookup("RECOMP_RES_SCALE", "resolution_scale");
 *
 * which keeps the precedence visible where the setting is read rather
 * than buried here.
 *
 * The file is per title, keyed by the title id out of the XBE
 * certificate: a resolution scale is a judgement about one game on one
 * machine, and the Hor+ register is meaningless to any other game. Input
 * bindings are deliberately not here -- those belong to the person, not
 * the title, and they already have a file of their own.
 *
 * It is an INI rather than JSON because the settings are flat and because
 * a generated INI can explain itself. The file a player opens says what
 * each line does; JSON has nowhere to put that.
 */
#ifndef XBOXRECOMP_RECOMP_CONFIG_H
#define XBOXRECOMP_RECOMP_CONFIG_H

#include <stddef.h>
#include <stdint.h>

/* The running title, from its XBE certificate, which decides the file's
 * name. Called during start-up before anything reads a setting; without
 * it the lookups below still work and simply find no file. */
void recomp_config_set_title(uint32_t title_id);
/* The title id given above, 0 before it is known. */
uint32_t recomp_config_title_id(void);
/* The per-user directories, the one place each is decided. Each returns 1
 * and writes the path (no trailing separator), or 0 if the host has no home
 * to put it in. None creates anything (recomp_make_dirs does), and none needs
 * SDL or any other initialisation: they read the environment only.
 * RECOMP_USER_DIR=<dir> overrides all three (user and data are <dir>, cache
 * is <dir>/cache), for tests and scripted runs.
 *
 *   user  settings and input bindings
 *         Windows %APPDATA%\xboxrecomp; macOS ~/Library/Application Support/
 *         xboxrecomp; Linux $XDG_CONFIG_HOME (or ~/.config)/xboxrecomp
 *   data  saves -- the FATX partition images and T:/U:/Z: -- unless
 *         RECOMP_SAVE_DIR says otherwise (kernel_path.c reads that)
 *         Windows %LOCALAPPDATA%\xboxrecomp; macOS as user; Linux
 *         $XDG_DATA_HOME (or ~/.local/share)/xboxrecomp
 *   cache disposable, rebuilt when missing (the shader cache)
 *         Windows as user; macOS ~/Library/Caches/xboxrecomp; Linux
 *         $XDG_CACHE_HOME (or ~/.cache)/xboxrecomp */
int recomp_config_user_dir(char *out, size_t n);
int recomp_data_dir(char *out, size_t n);
int recomp_cache_dir(char *out, size_t n);
/* mkdir -p: every missing directory on the way to path. 1 if it exists
 * afterwards. */
int recomp_make_dirs(const char *path);

/* The value for a setting: the environment variable if it is set and not
 * empty, then the config file, then NULL. Either name may be NULL to skip
 * that source. The returned string is owned here and valid for the life
 * of the process. */
const char *recomp_config_lookup(const char *env_name, const char *key);

/* The same, parsed, with a fallback for absent or unreadable values.
 * Booleans accept 1/0, on/off, yes/no, true/false, and an empty
 * environment variable counts as on -- the rule xbox_EnvSwitch already
 * uses, so a variable behaves the same whether or not this file exists. */
int    recomp_config_int(const char *env_name, const char *key, int fallback);
int    recomp_config_bool(const char *env_name, const char *key, int fallback);
double recomp_config_float(const char *env_name, const char *key, double fallback);

/* The file the lookups read, for a log line or a launcher; NULL when no
 * file was found. */
const char *recomp_config_path(void);

/* ---------------------------------------------------------- widescreen */

/* What "widescreen on" means is a fact about the title, not a choice.
 *
 * Xbox widescreen is anamorphic: the console says it is 16:9, and a title
 * with a 16:9 mode of its own widens its camera and squeezes the picture
 * into the same 640x480 for the display to stretch back. For that title
 * the flag and a 16:9 presentation are the whole job (Burnout 2, OutRun 2).
 * A title with no such mode (TimeSplitters 2) ignores the flag and draws
 * 4:3, which presented at 16:9 is the same view stretched; it needs its
 * camera widened as well -- Hor+ -- and the register its projection is in.
 *
 * Those used to be two switches in the launcher, and choosing the wrong
 * pair gave a stretched picture or a view widened twice. Now the player
 * has one, `widescreen`, and the rest is resolved here when the game
 * starts, from what the title's project has said about it. */
typedef enum RecompWideMode {
    RECOMP_WIDE_UNKNOWN = 0,    /* nothing said: resolved as NATIVE */
    RECOMP_WIDE_NATIVE,         /* a 16:9 mode of its own, on the console flag */
    RECOMP_WIDE_HOR_PLUS        /* none: its camera has to be widened */
} RecompWideMode;

/* What the title's project says. Three ways to say it, all ending here:
 *
 *   recomp_title_widescreen(HOR_PLUS REGISTER 60) in the title's
 *       CMakeLists (src/config/CMakeLists.txt): compiled in, so the game
 *       and its launcher both know before anything runs. The usual way.
 *   this call, from the title's own code, before its device is created.
 *   xbox_D3D8ClaimHorPlus (src/d3d/d3d8_xbox.h): a title that widens its
 *       own camera has thereby said it has no 16:9 mode.
 *
 * `projection_register` is the vertex constant register holding the first
 * row of the title's projection, or a negative number to leave what was
 * said before. It matters to a NATIVE title too: the register is how the
 * renderer tells a 3D draw from a screen-space one in widescreen. */
void           recomp_widescreen_title(RecompWideMode mode, int projection_register);
RecompWideMode recomp_widescreen_title_mode(void);
/* Whether the title's project asked for the centre 2D policy by default
 * (recomp_title_widescreen ... CENTRE_2D); widescreen_2d overrides it. */
int            recomp_widescreen_title_centre_2d(void);
/* Changes whenever the above is called, so a caller that keeps the
 * resolution below knows to ask again. */
unsigned       recomp_widescreen_title_serial(void);

/* The 4:3-to-16:9 Hor+ factor, (4/3)/(16/9): the projection's x scale is
 * multiplied by it, which widens the horizontal field of view and leaves
 * the vertical alone. */
#define RECOMP_HOR_PLUS_16_9   0.75
/* The projection register when nobody names one: TimeSplitters 2's, and
 * the value every settings file was written with before a title could
 * name its own. */
#define RECOMP_PROJECTION_REGISTER_DEFAULT 60

typedef struct RecompWidescreen {
    int            on;              /* the console flag, and a 16:9 presentation */
    double         hor_plus;        /* x scale of the projection; 1 leaves the camera alone */
    int            hor_plus_named;  /* hor_plus was set outright, not resolved from `on` */
    int            projection_register;
    int            register_named;  /* likewise hor_plus_register */
    RecompWideMode mode;            /* what the title said */
    int            title_register;  /* the register the title named, or -1 */
} RecompWidescreen;

/* The resolution, for the running title: the settings (environment, then
 * file) over what the title said, over the defaults.
 *
 *   on        RECOMP_WIDESCREEN / widescreen, off by default.
 *   hor_plus  RECOMP_HOR_PLUS / hor_plus if it is a number -- 0 leaves the
 *             camera alone whatever the title, and a factor applies even
 *             with widescreen off, as both always have. Absent or "auto":
 *             0.75 when widescreen is on and the title is HOR_PLUS,
 *             nothing otherwise.
 *   register  RECOMP_HOR_PLUS_REG / hor_plus_register if it is a number,
 *             else the title's, else 60.
 *
 * An UNKNOWN title resolves as NATIVE: the flag and 16:9, no Hor+. That
 * is what `widescreen = 1` alone has always done, it is right for every
 * title with a mode of its own, and the alternative is worse -- scaling
 * c[60] of a title nobody has looked at corrupts whatever it keeps there,
 * and widens a title with its own mode twice. The price is that a 4:3-only
 * title nobody has described looks stretched until someone does. */
void recomp_widescreen_resolve(RecompWidescreen *out);

/* The same from its inputs as text, with nothing read from the process:
 * what the tests drive. `hor_plus` and `reg` are the settings' values,
 * NULL when absent. */
void recomp_widescreen_resolve_from(int on, const char *hor_plus, const char *reg,
                                    RecompWideMode mode, int title_register,
                                    RecompWidescreen *out);

/* ------------------------------------------------------------ settings */

/* hor_plus and hor_plus_register left for the title to decide. Written to
 * the file as a commented-out line rather than a word, so a build from
 * before "auto" reads the file as it always read a missing key. */
#define RECOMP_HOR_PLUS_AUTO       (-1.0)
#define RECOMP_REGISTER_AUTO       (-1)

/* The settings a player chooses, as one thing, so the launcher that
 * writes the file and the runtime that reads it cannot drift apart on
 * what the file contains. The runtime still reads values one at a time
 * through the lookups above -- that keeps the environment override
 * visible where each setting is used -- but the file's shape is here. */
typedef struct RecompSettings {
    int  resolution_scale;      /* 1..8 */
    int  widescreen;            /* 0 or 1: the one switch; see RecompWidescreen */
    double hor_plus;            /* RECOMP_HOR_PLUS_AUTO, or an override: 0 leaves
                                 * the camera alone, 0.75 is 4:3->16:9 */
    int  hor_plus_register;     /* RECOMP_REGISTER_AUTO, or an override: 0..191 */
    char widescreen_2d[16];     /* "auto" or "centre": where 2D goes in widescreen */
    int  anisotropy;            /* 1..16 */
    char frame_cap[16];         /* "adaptive", "60", "30", "0" */
    int  fps_overlay;           /* 0 or 1 */
    int  fullscreen;            /* 0 or 1: borderless, on the window's monitor */
    int  vrr;                   /* 0 or 1: present for variable refresh when fullscreen */
    int  frame_interp;          /* 1 (off) to 4: frames shown for each the game draws */
    char game_dir[512];         /* empty means beside the executable */

    /* System Link (src/kernel/xbox_net_udp.c). */
    char syslink[8];            /* "off", "host", "join"; "udp" by hand only */
    char syslink_host[128];     /* join: the host's address or name, [:port] */
    int  syslink_port;          /* the port a host listens on, and joins use */
    char syslink_code[64];      /* the session's join code; empty for none */
    int  syslink_upnp;          /* host: ask the router to forward the port */
} RecompSettings;

/* The port a System Link host listens on when nobody chooses one. The same
 * number as XBOX_NET_DEFAULT_PORT, which the runtime falls back to. */
#define RECOMP_SYSLINK_DEFAULT_PORT 27610

void recomp_settings_defaults(RecompSettings *s);

/* Read the file at `path` into `s`, starting from the defaults. Absent
 * keys keep their default, so an older file gains new settings rather
 * than losing them. Returns 0 if the file could not be read at all. */
int  recomp_settings_read(const char *path, RecompSettings *s);

/* Write `s` to `path`, comments and all, creating the directories above
 * it. Returns 0 on failure. This is the only place the file's text is
 * written, by the launcher and by the runtime's first-run default
 * alike. */
int  recomp_settings_write(const char *path, const RecompSettings *s);

/* The launcher's one widescreen switch, applied to settings read from a
 * file that may hold more than the switch: a hor_plus someone set by hand,
 * or the 0 / 0.75 an older launcher wrote from its second switch.
 *
 * A switch the player moved is the player asking for widescreen done
 * right, so hor_plus goes back to auto. A switch left where it was leaves
 * hor_plus as the file had it, so pressing Play never undoes an override
 * and never changes what an older file already meant.
 *
 * The register is not the switch's to change -- except that 60 is what
 * every file held before a title could name its own, whatever the game,
 * so 60 is written back as auto: the same register for a title that says
 * nothing, and no longer in the way of one that does. */
void recomp_settings_set_widescreen(RecompSettings *s, int on);

/* Where this title's file belongs: RECOMP_DISPLAY_CONFIG if set,
 * otherwise the per-user path for `title_id`. Returns 0 if there is
 * nowhere sensible to put it. */
int  recomp_settings_path(uint32_t title_id, char *out, size_t n);

#endif /* XBOXRECOMP_RECOMP_CONFIG_H */
