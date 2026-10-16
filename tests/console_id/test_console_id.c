/*
 * The console identity loader (src/kernel/xbox_console_id.c), on a made-up
 * EEPROM and hddinfo.txt written here: the checksums, and where each field
 * is read from. Portable; no real console's data.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xbox_console_id.h"

#ifdef _WIN32
#include <direct.h>
#define SEP "\\"
static int make_dir(const char *p) { return _mkdir(p); }
static void set_env(const char *k, const char *v) { _putenv_s(k, v); }
#else
#include <sys/stat.h>
#define SEP "/"
static int make_dir(const char *p) { return mkdir(p, 0700); }
static void set_env(const char *k, const char *v) { setenv(k, v, 1); }
#endif

/* The loader looks beside the game folder by default; this test names the
 * folder outright, so the game folder is never asked for. */
int xbox_path_game_dir_utf8(char *out, size_t n)
{
    (void)out; (void)n;
    return 0;
}

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

int main(int argc, char **argv)
{
    unsigned char ee[256];
    char dir[512], path[600];
    const XboxConsoleId *id;
    FILE *f;
    int i;

    snprintf(dir, sizeof dir, "%s", argc > 1 ? argv[1] : "console_id_test");
    make_dir(dir);

    /* The known answer: an all-zero section sums to 0, complemented. */
    memset(ee, 0, sizeof ee);
    CHECK(xbox_eeprom_checksum(ee, 0x2C) == 0xFFFFFFFFu, "checksum of zeros");
    /* A carry out of the low dword is added back in. */
    put32(ee, 0xFFFFFFFFu);
    put32(ee + 4, 0x00000002u);
    CHECK(xbox_eeprom_checksum(ee, 8) == ~(0x00000001u + 1u), "checksum carry");

    /* A made-up console. */
    memset(ee, 0, sizeof ee);
    for (i = 0; i < 0x30; i++)
        ee[i] = (unsigned char)(0xA0 + i);            /* "encrypted" section */
    memcpy(ee + 0x34, "123456789012", 12);
    memcpy(ee + 0x40, "\x00\x12\x5A\x01\x02\x03", 6);
    for (i = 0; i < 16; i++)
        ee[0x48 + i] = (unsigned char)(0x10 + i);     /* online key */
    put32(ee + 0x58, 0x00400100u);                    /* video standard */
    put32(ee + 0x30, xbox_eeprom_checksum(ee + 0x34, 0x2C));
    ee[0x64] = 1;                                     /* something in the user section */
    put32(ee + 0x60, xbox_eeprom_checksum(ee + 0x64, 0x5C));

    snprintf(path, sizeof path, "%s" SEP "eeprom.bin", dir);
    f = fopen(path, "wb");
    fwrite(ee, 1, sizeof ee, f);
    fclose(f);
    snprintf(path, sizeof path, "%s" SEP "hddinfo.txt", dir);
    f = fopen(path, "w");
    fprintf(f, "Disk Serial Number : TESTSERIAL01\n"
               "Disk Model Number  : TEST DRIVE 8GB\n"
               "HDD Key            : (shown in a block EvoX writes)\n"
               "\n"
               "HDDLockDisablePassword 00 11 22 33 44 55 66 77 88 99 aa bb cc dd ee ff "
               "00 11 22 33 00 00 00 00 00 00 00 00 00 00 00 00\n"
               "\n"
               "XboxHDKey 01 23 45 67 89 ab cd ef fe dc ba 98 76 54 32 10\n");
    fclose(f);

    set_env("RECOMP_CONSOLE_DIR", dir);
    id = xbox_ConsoleId();
    CHECK(id != NULL, "a good EEPROM was not used");
    if (id) {
        static const unsigned char key[16] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                                               0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10 };
        CHECK(memcmp(id->serial, "123456789012", 12) == 0, "serial");
        CHECK(memcmp(id->mac, "\x00\x12\x5A\x01\x02\x03", 6) == 0, "Ethernet address");
        CHECK(id->online_key[0] == 0x10 && id->online_key[15] == 0x1F, "online key");
        CHECK(id->have_hd_key && memcmp(id->hd_key, key, 16) == 0,
              "HDD key from XboxHDKey, not the lock password");
        CHECK(strcmp(id->disk_serial, "TESTSERIAL01") == 0, "disk serial '%s'", id->disk_serial);
        CHECK(strcmp(id->disk_model, "TEST DRIVE 8GB") == 0, "disk model '%s'", id->disk_model);
        CHECK(id->eeprom[0] == 0xA0, "the whole EEPROM kept");
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("console_id: all checks passed\n");
    return 0;
}
