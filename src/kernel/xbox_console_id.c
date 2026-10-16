/**
 * The console's own identity, from the player's backup of a real console.
 *
 * Without one, every install is the same made-up console: serial
 * 100000000000, a random Ethernet address per save folder, and zeroed online
 * and hard disk keys. That is enough for System Link between recompiled
 * builds. Xbox Live is not: the title's own XOnline code creates the
 * console's machine account from the serial and the online key, and
 * Insignia registers a console by its keys and refuses one whose hard disk
 * key is zero. So the identity has to be a real console's, the player's own,
 * which they back up from their console (EvoX, UnleashX, XBMC: an
 * eeprom.bin, and the HDD key the same tools show).
 *
 * Where it is looked for, first found wins:
 *
 *     console_dir = <folder>        [RECOMP_CONSOLE_DIR]   (off: none at all)
 *     <the game folder>/../xboxbackup/
 *     <the per-user settings folder>/console/
 *
 * In that folder:
 *
 *     eeprom.bin    256 bytes, required. Its factory and user sections must
 *                   carry good checksums, or it is not used.
 *     hddinfo.txt   EvoX's, read for its "XboxHDKey" line and the drive's
 *                   serial and model.
 *     hd_key.txt    or the HDD key alone, 32 hex digits.
 *
 * and the HDD key from keys.ini (hd_key = ...) or RECOMP_XBOX_HD_KEY wins
 * over both. The key in eeprom.bin itself is encrypted with the BIOS's
 * EEPROM key, which is not needed when the decrypted key is given.
 *
 * And disk.bin, the first 512 KB of the console's drive (EvoX), whose
 * Xbox Live accounts are copied onto the runtime's disk image once
 * (xbox_console_import_accounts).
 *
 * What it changes, when found: the serial, Ethernet address and online key
 * ExQueryNonVolatileSetting answers (kernel_xbox.c), XboxHDKey and the HAL's
 * disk serial and model (kernel_bridge.c), and the card's Ethernet address
 * (xbox_nic.c; RECOMP_SYSLINK_MAC still wins). A save the title signs with a
 * console-bound key is then signed as that console signs it, so saves copied
 * from it verify, and saves made earlier under the zero key may not.
 *
 * All of it is secret to one console. Nothing here prints a key, the serial
 * or the full Ethernet address, only where they came from.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xbox_console_id.h"
#include "kernel.h"          /* xbox_path_game_dir_utf8 */
#include "recomp_config.h"

#ifdef _WIN32
#define SEP "\\"
#define strtok_r strtok_s
#else
#include <strings.h>
#define SEP "/"
#define _stricmp strcasecmp
#endif

/* EEPROM layout (xboxdevwiki "EEPROM"): an encrypted security section, then
 * the factory section and the user section, each led by its checksum. */
#define EE_FACTORY_SUM   0x30
#define EE_FACTORY       0x34
#define EE_FACTORY_END   0x60
#define EE_SERIAL        0x34
#define EE_MAC           0x40
#define EE_ONLINE_KEY    0x48
#define EE_USER_SUM      0x60
#define EE_USER          0x64
#define EE_USER_END      0xC0

uint32_t xbox_eeprom_checksum(const uint8_t *data, uint32_t len)
{
    uint32_t hi = 0, lo = 0, i;
    uint64_t s;

    for (i = 0; i + 4 <= len; i += 4) {
        uint32_t w = (uint32_t)data[i] | (uint32_t)data[i + 1] << 8 |
                     (uint32_t)data[i + 2] << 16 | (uint32_t)data[i + 3] << 24;
        s = (uint64_t)lo + w;
        lo = (uint32_t)s;
        hi += (uint32_t)(s >> 32);
    }
    s = (uint64_t)hi + lo;
    return ~(uint32_t)(s + (s >> 32));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int is_dir_with_eeprom(const char *dir)
{
    char path[1100];
    FILE *f;

    snprintf(path, sizeof path, "%s" SEP "eeprom.bin", dir);
    f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 16 bytes of hex, with or without spaces between the pairs. */
static int parse_key16(const char *s, uint8_t out[16])
{
    int n = 0;

    while (*s && n < 16) {
        int hi, lo;
        while (*s == ' ' || *s == '\t' || *s == ':' || *s == '-')
            s++;
        hi = hexval((unsigned char)s[0]);
        lo = hi < 0 ? -1 : hexval((unsigned char)s[1]);
        if (lo < 0)
            break;
        out[n++] = (uint8_t)(hi << 4 | lo);
        s += 2;
    }
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    return n == 16;
}

/* The value after `name` on a line of a key = value or "Name : value" file. */
static int line_value(const char *line, const char *name, const char **value)
{
    size_t n = strlen(name);

    while (*line == ' ' || *line == '\t')
        line++;
    if (strncmp(line, name, n) != 0)
        return 0;
    line += n;
    while (*line == ' ' || *line == '\t')
        line++;
    if (*line == '=' || *line == ':')
        line++;
    while (*line == ' ' || *line == '\t')
        line++;
    *value = line;
    return 1;
}

static void trim_copy(char *out, size_t n, const char *v)
{
    size_t len;

    snprintf(out, n, "%s", v);
    len = strlen(out);
    while (len && isspace((unsigned char)out[len - 1]))
        out[--len] = 0;
}

static int key_from_file(const char *path, const char *name, uint8_t out[16])
{
    char line[512];
    FILE *f = fopen(path, "r");
    int found = 0;

    if (!f)
        return 0;
    while (!found && fgets(line, sizeof line, f)) {
        const char *v;
        if (name ? line_value(line, name, &v) : (v = line, 1))
            found = parse_key16(v, out);
    }
    fclose(f);
    return found;
}

static void read_hddinfo(XboxConsoleId *id, const char *path, const char **key_source)
{
    char line[512];
    FILE *f = fopen(path, "r");

    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        const char *v;
        if (line_value(line, "XboxHDKey", &v) && !id->have_hd_key &&
            parse_key16(v, id->hd_key)) {
            id->have_hd_key = 1;
            *key_source = "hddinfo.txt";
        } else if (line_value(line, "Disk Serial Number", &v)) {
            trim_copy(id->disk_serial, sizeof id->disk_serial, v);
        } else if (line_value(line, "Disk Model Number", &v)) {
            trim_copy(id->disk_model, sizeof id->disk_model, v);
        }
    }
    fclose(f);
}

/* The folder holding `path`, worked out from the text: "." and ".." are
 * collapsed as written, so a game folder that is a symlink (game -> the
 * toolkit's games/<title>) still has the project as its parent, where the
 * OS, following the link, would say the toolkit's games/. */
static void lexical_parent(const char *path, char *out, size_t n)
{
    char buf[1024], *parts[128], *p, *save = NULL;
    int count = 0, i, absolute;
    size_t len;

    snprintf(buf, sizeof buf, "%s", path);
    for (p = buf; *p; p++)
        if (*p == '\\')
            *p = '/';
    absolute = buf[0] == '/';
    for (p = strtok_r(buf, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
        if (strcmp(p, ".") == 0)
            continue;
        if (strcmp(p, "..") == 0 && count > 0 && strcmp(parts[count - 1], "..") != 0) {
            count--;
            continue;
        }
        if (count < (int)(sizeof parts / sizeof parts[0]))
            parts[count++] = p;
    }
    if (count > 0)
        count--;                          /* the parent */
    out[0] = 0;
    len = 0;
    if (absolute)
        len += (size_t)snprintf(out, n, "/");
    for (i = 0; i < count && len < n; i++)
        len += (size_t)snprintf(out + len, n - len, "%s%s", i ? SEP : "", parts[i]);
    if (!out[0])
        snprintf(out, n, ".");
}

static int find_dir(char *out, size_t n, const char **how)
{
    const char *v = recomp_config_lookup("RECOMP_CONSOLE_DIR", "console_dir");
    char base[1024];

    if (v && *v) {
        if (_stricmp(v, "off") == 0 || strcmp(v, "0") == 0) {
            *how = "off";
            return 0;
        }
        snprintf(out, n, "%s", v);
        *how = "console_dir";
        return is_dir_with_eeprom(out);
    }
    if (xbox_path_game_dir_utf8(base, sizeof base)) {
        char parent[1024];
        lexical_parent(base, parent, sizeof parent);
        snprintf(out, n, "%s" SEP "xboxbackup", parent);
        if (is_dir_with_eeprom(out)) {
            *how = "beside the game folder";
            return 1;
        }
    }
    if (recomp_config_user_dir(base, sizeof base)) {
        snprintf(out, n, "%s" SEP "console", base);
        if (is_dir_with_eeprom(out)) {
            *how = "the settings folder";
            return 1;
        }
    }
    *how = NULL;
    return 0;
}

static int load(XboxConsoleId *id)
{
    char path[1100], dir[1024];
    const char *how = NULL, *key_source = NULL, *env;
    FILE *f;
    size_t got;
    int i;

    memset(id, 0, sizeof *id);
    if (!find_dir(dir, sizeof dir, &how)) {
        if (how && strcmp(how, "off") == 0)
            fprintf(stderr, "[XBOX] console identity: off (console_dir = off)\n");
        else if (how)
            fprintf(stderr, "[XBOX] console identity: no eeprom.bin in console_dir "
                            "%s; the made-up console instead\n", dir);
        return 0;
    }
    snprintf(id->dir, sizeof id->dir, "%s", dir);

    snprintf(path, sizeof path, "%s" SEP "eeprom.bin", dir);
    f = fopen(path, "rb");
    got = f ? fread(id->eeprom, 1, sizeof id->eeprom, f) : 0;
    if (f && fgetc(f) != EOF)
        got = 0;                          /* longer than an EEPROM */
    if (f)
        fclose(f);
    if (got != sizeof id->eeprom) {
        fprintf(stderr, "[XBOX] console identity: %s is not a 256-byte EEPROM; "
                        "not used\n", path);
        return 0;
    }
    if (xbox_eeprom_checksum(id->eeprom + EE_FACTORY, EE_FACTORY_END - EE_FACTORY) !=
            le32(id->eeprom + EE_FACTORY_SUM) ||
        xbox_eeprom_checksum(id->eeprom + EE_USER, EE_USER_END - EE_USER) !=
            le32(id->eeprom + EE_USER_SUM)) {
        fprintf(stderr, "[XBOX] console identity: %s fails its checksums (not an "
                        "EEPROM backup, or damaged); not used\n", path);
        return 0;
    }
    memcpy(id->serial, id->eeprom + EE_SERIAL, 12);
    for (i = 0; i < 12; i++)
        if (!isdigit((unsigned char)id->serial[i])) {
            fprintf(stderr, "[XBOX] console identity: %s has no serial number; "
                            "not used\n", path);
            return 0;
        }
    memcpy(id->mac, id->eeprom + EE_MAC, 6);
    memcpy(id->online_key, id->eeprom + EE_ONLINE_KEY, 16);

    /* The HDD key: the environment, keys.ini, then the backup's own files. */
    env = getenv("RECOMP_XBOX_HD_KEY");
    if (env && *env) {
        if (parse_key16(env, id->hd_key)) {
            id->have_hd_key = 1;
            key_source = "RECOMP_XBOX_HD_KEY";
        } else {
            fprintf(stderr, "[XBOX] RECOMP_XBOX_HD_KEY is not 32 hex digits; ignored\n");
        }
    }
    if (!id->have_hd_key) {
        char udir[1024];
        if (recomp_config_user_dir(udir, sizeof udir)) {
            snprintf(path, sizeof path, "%s" SEP "keys.ini", udir);
            if (key_from_file(path, "hd_key", id->hd_key)) {
                id->have_hd_key = 1;
                key_source = "keys.ini";
            }
        }
    }
    snprintf(path, sizeof path, "%s" SEP "hddinfo.txt", dir);
    read_hddinfo(id, path, &key_source);
    if (!id->have_hd_key) {
        snprintf(path, sizeof path, "%s" SEP "hd_key.txt", dir);
        if (key_from_file(path, NULL, id->hd_key)) {
            id->have_hd_key = 1;
            key_source = "hd_key.txt";
        }
    }
    if (id->have_hd_key) {
        static const uint8_t zero[16];
        if (memcmp(id->hd_key, zero, 16) == 0)
            fprintf(stderr, "[XBOX] console identity: the HDD key is all zeros; "
                            "Insignia refuses such a console\n");
    }

    fprintf(stderr, "[XBOX] console identity from %s (%s): eeprom.bin, Ethernet "
                    "address %02X:%02X:%02X:..., %s%s%s\n",
            dir, how, id->mac[0], id->mac[1], id->mac[2],
            id->have_hd_key ? "HDD key from " : "no HDD key (keys.ini hd_key, or "
                                                "hddinfo.txt beside eeprom.bin)",
            id->have_hd_key ? key_source : "",
            id->disk_serial[0] ? ", and the drive's serial and model" : "");
    fflush(stderr);
    return 1;
}

const XboxConsoleId *xbox_ConsoleId(void)
{
    static XboxConsoleId id;
    static int state;        /* 0 not looked, 1 found, -1 none */

    if (!state)
        state = load(&id) ? 1 : -1;
    return state > 0 ? &id : NULL;
}

/* ---- the Xbox Live accounts ------------------------------------------- */

#define ACCT_FIRST  8           /* the index */
#define ACCT_LAST   15
#define SECTOR      512u

/* What an account sector holds: 0 nothing, 1 the machine account (named
 * SN.<serial>), 2 a user account. The record starts 0x0C into its sector,
 * its name at +0x0C, its Kerberos domain at +0x24. */
static int account_kind(const uint8_t *sec)
{
    const uint8_t *rec = sec + 0x0C;
    uint32_t i;

    for (i = 0; i < SECTOR && !sec[i]; i++)
        ;
    if (i == SECTOR)
        return 0;
    if (!rec[0x24])
        return 0;               /* no domain: not an account record */
    return memcmp(rec + 0x0C, "SN.", 3) == 0 ? 1 : 2;
}

static int any_account(const uint8_t *area)
{
    uint32_t s;

    for (s = ACCT_FIRST + 1; s <= ACCT_LAST; s++)
        if (account_kind(area + (s - ACCT_FIRST) * SECTOR))
            return 1;
    return 0;
}

void xbox_console_import_accounts(const char *image_path)
{
    enum { N = (ACCT_LAST - ACCT_FIRST + 1) * SECTOR };
    static uint8_t from[N], onto[N];
    const XboxConsoleId *id = xbox_ConsoleId();
    const char *mode = recomp_config_lookup("RECOMP_CONSOLE_ACCOUNTS", "console_accounts");
    int replace = mode && _stricmp(mode, "replace") == 0;
    int machine = 0, users = 0;
    char path[1100];
    uint32_t s;
    FILE *f;
    size_t got;

    if (!id || !image_path || (mode && _stricmp(mode, "off") == 0))
        return;
    snprintf(path, sizeof path, "%s" SEP "disk.bin", id->dir);
    f = fopen(path, "rb");
    if (!f)
        return;                 /* no drive backup: nothing to bring */
    got = fseek(f, (long)(ACCT_FIRST * SECTOR), SEEK_SET) == 0 ? fread(from, 1, N, f) : 0;
    fclose(f);
    if (got != N || !any_account(from)) {
        fprintf(stderr, "[XBOX] Xbox Live accounts: none in %s (a backup from "
                        "before signing up?)\n", path);
        return;
    }

    f = fopen(image_path, "r+b");
    if (!f) {
        fprintf(stderr, "[XBOX] Xbox Live accounts: cannot open %s\n", image_path);
        return;
    }
    got = fseek(f, (long)(ACCT_FIRST * SECTOR), SEEK_SET) == 0 ? fread(onto, 1, N, f) : 0;
    if (got == N && any_account(onto) && !replace) {
        fclose(f);
        /* The index (sector 8) is the title's to update every run; only
         * different accounts are worth a word. */
        if (memcmp(onto + SECTOR, from + SECTOR, N - SECTOR) != 0)
            fprintf(stderr, "[XBOX] Xbox Live accounts: the disk already has its "
                            "own, kept (console_accounts = replace takes "
                            "disk.bin's)\n");
        return;
    }
    if (fseek(f, (long)(ACCT_FIRST * SECTOR), SEEK_SET) != 0 ||
        fwrite(from, 1, N, f) != N) {
        fclose(f);
        fprintf(stderr, "[XBOX] Xbox Live accounts: cannot write %s\n", image_path);
        return;
    }
    fclose(f);
    for (s = ACCT_FIRST + 1; s <= ACCT_LAST; s++) {
        int k = account_kind(from + (s - ACCT_FIRST) * SECTOR);
        machine += k == 1;
        users += k == 2;
    }
    fprintf(stderr, "[XBOX] Xbox Live accounts copied from disk.bin onto the "
                    "disk (sectors %d-%d): %d machine account, %d user "
                    "account%s\n", ACCT_FIRST, ACCT_LAST, machine, users,
            users == 1 ? "" : "s");
    fflush(stderr);
}
