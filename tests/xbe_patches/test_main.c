/*
 * xbe_patches - an XBE patched before the lift, loaded unpatched.
 *
 *   xbe_patches_test <original.xbe> <patched.xbe>
 *
 * The fixture (make_fixture.py, built by CMake) is tools/xbe_patch's synthetic
 * XBE with a table copied into an added section, an instruction repointed at
 * it and a count raised; recomp_xbe_patches.c, generated from it as the lift
 * generates it, is linked into this test as it is into a title. Checked:
 * patches for another file refuse to load, and the real
 * xbox_MemoryLayoutInit, given the unpatched file, maps the image that was
 * lifted -- the added section at its VA with the copied table in it, the
 * patched code bytes, and a guest header identical to the patched file's.
 * POSIX (setenv/_exit); no game files.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "xbe_patches.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* _exit */

void *recomp_lookup(ULONG address) { (void)address; return NULL; }
void *recomp_lookup_manual(ULONG address) { (void)address; return NULL; }

extern ptrdiff_t g_xbox_mem_offset;

static int failures;
#define CHECK(name, cond) \
    do { if (cond) {} else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } } while (0)

static uint8_t *G(uint32_t va) { return (uint8_t *)g_xbox_mem_offset + va; }

static uint8_t *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *data;
    long n;
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (uint8_t *)malloc((size_t)n);
    if (fread(data, 1, (size_t)n, f) != (size_t)n) { perror(path); exit(2); }
    fclose(f);
    *size = (size_t)n;
    return data;
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

int main(int argc, char **argv)
{
    size_t orig_size, want_size, out_size;
    uint8_t *orig, *want, *out = NULL;
    uint32_t hdr_size;

    if (argc != 3) {
        printf("usage: xbe_patches_test <original.xbe> <patched.xbe>\n");
        return 2;
    }
    orig = slurp(argv[1], &orig_size);
    want = slurp(argv[2], &want_size);

    /* No patches: nothing to do. */
    CHECK("no blob, nothing applied",
          xbox_xbe_patches_apply_blob(NULL, 0, orig, orig_size, &out, &out_size) == 0
          && out == NULL);

    /* The patches rebuild the patched file byte for byte. */
    CHECK("applies to the original", xbox_xbe_patches_apply(orig, orig_size, &out, &out_size) == 1);
    CHECK("gives the lifted image", out && out_size == want_size
                                    && memcmp(out, want, want_size) == 0);
    free(out);

    /* Another XBE (one byte different) is refused: the lifted code would
     * find the patched bytes in the wrong places. */
    orig[0x1100] ^= 0xFF;
    CHECK("another XBE refused", xbox_xbe_patches_apply(orig, orig_size, &out, &out_size) == -1
                                 && out == NULL);
    CHECK("and refused by the layout", !xbox_MemoryLayoutInit(orig, orig_size));
    orig[0x1100] ^= 0xFF;

    setenv("RECOMP_VBLANK", "1", 1);
    setenv("RECOMP_AC97_READY", "1", 1);
    CHECK("layout init", xbox_MemoryLayoutInit(orig, orig_size));
    if (failures) {
        printf("xbe_patches: %d FAILURE(S)\n", failures);
        return 1;
    }

    /* The guest sees the patched image: its header, which names the added
     * section and the larger image... */
    hdr_size = rd32(want + 0x108);
    CHECK("guest header is the patched file's", memcmp(G(0x10000), want, hdr_size) == 0);
    CHECK("four sections", rd32(G(0x1011C)) == 4);
    /* ...the added section at 0x1C000 with the table copied into it from
     * .rdata, zeros copied from .data's BSS, and the fill and write... */
    CHECK("table copied into the added section",
          memcmp(G(0x1C000), G(0x19000), 0x40) == 0 && G(0x1C000)[0] == 1
          && G(0x1C03F)[0] == 0x40);
    CHECK("BSS copy is zeros", G(0x1C100)[0] == 0 && G(0x1C11F)[0] == 0);
    CHECK("fill", rd32(G(0x1C200)) == 0xAAAAAAAAu);
    CHECK("write", memcmp(G(0x1C204), "\xDE\xAD\xBE\xEF", 4) == 0);
    CHECK("rest of the section zero", G(0x1C208)[0] == 0 && G(0x1CFFF)[0] == 0);
    /* ...and the code bytes that were lifted. */
    CHECK("code repointed at the copy", rd32(G(0x11003)) == 0x0001C000u);
    CHECK("count raised", G(0x11009)[0] == 0x7E);

    if (failures) {
        printf("xbe_patches: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("xbe_patches: ALL PASS\n");
    fflush(stdout);
    _exit(0);   /* the layout's ack thread is still running; no teardown */
}
