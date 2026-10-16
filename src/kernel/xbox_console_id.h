/**
 * The console's own identity, from the player's backup of a real console:
 * its EEPROM (serial number, Ethernet address, online key) and its hard
 * disk key. Xbox Live (Insignia) knows a console by these.
 *
 * See xbox_console_id.c for where it is looked for.
 */
#ifndef XBOX_CONSOLE_ID_H
#define XBOX_CONSOLE_ID_H

#include <stdint.h>

typedef struct XboxConsoleId {
    char     dir[1024];         /* where it was found */
    uint8_t  eeprom[256];       /* the whole EEPROM, as the console holds it */
    char     serial[12];        /* twelve ASCII digits, no terminator */
    uint8_t  mac[6];
    uint8_t  online_key[16];
    int      have_hd_key;
    uint8_t  hd_key[16];        /* XboxHDKey, decrypted */
    char     disk_serial[24];   /* the drive's own, "" if not known */
    char     disk_model[48];
} XboxConsoleId;

/* This console's identity, loaded the first time it is asked for; NULL when
 * there is none (no backup, or console_dir = off). Ask after xbox_path_init,
 * since the default place is beside the game folder. */
const XboxConsoleId *xbox_ConsoleId(void);

/* Copy the console's Xbox Live accounts into the runtime's disk image, once.
 *
 * XOnline keeps them in the hard disk's config area, which the runtime backs
 * with Partition0.img: an index in sector 8, the machine account in sector 9
 * and user accounts after it. EvoX's disk.bin is the first 512 KB of the
 * console's drive, so a backup taken after signing up carries them. Sectors
 * 8 to 15 are copied from disk.bin when the image holds no account yet
 * (sectors 9 to 15 empty); accounts already on the image are kept.
 *
 *     console_accounts = import | replace | off      [RECOMP_CONSOLE_ACCOUNTS]
 *
 * replace takes disk.bin's even over accounts on the image (a newer backup).
 * Call after xbox_path_init has made the images. image_path is Partition0.img. */
void xbox_console_import_accounts(const char *image_path);

/* The checksum the EEPROM's factory and user sections carry: a 64-bit sum of
 * little-endian dwords, folded and complemented. For tests. */
uint32_t xbox_eeprom_checksum(const uint8_t *data, uint32_t len);

#endif /* XBOX_CONSOLE_ID_H */
