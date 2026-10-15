/*
 * XBE patches at load (xbe_patches.h, docs/technical/xbe-patches.md).
 *
 * The operations themselves -- add a section, patch, copy, fill -- are carried
 * out once, by tools/xbe_patch, on the XBE the pipeline lifts. What reaches
 * this file is their result as byte runs over the original file: apply them to
 * the default.xbe the player has and it becomes, byte for byte, the image that
 * was lifted, header included (the section table that names the new section,
 * the larger image size). The loader then maps it like any other XBE, so the
 * added section is mapped, the copied table is in it, and code bytes the
 * title reads match the code that was lifted -- all before the entry point.
 *
 * Blob layout (little-endian; tools.xbe_patch.build_blob):
 *   "XBEP", u32 version (1), u32 original size, u32 patched size,
 *   u8[20] SHA-1 of the original, u8[20] SHA-1 of the patched image,
 *   u32 run count, then per run: u32 file offset, u32 length, u8[length].
 */
#include "xbe_patches.h"
#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* No weak symbols on MSVC: an unresolved recomp_xbe_patches falls back to
 * this name instead, defined here so the fallback is always in the link. On
 * other compilers xbe_patches_none.c holds a weak default. */
#if defined(_MSC_VER)
#pragma comment(linker, "/alternatename:recomp_xbe_patches=recomp_xbe_patches_none")
const unsigned char *recomp_xbe_patches_none(unsigned int *size)
{
    *size = 0;
    return NULL;
}
#endif

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void sha1_of(const uint8_t *data, size_t size, unsigned char digest[20])
{
    XBOX_SHA_CONTEXT ctx;

    xbox_XcSHAInit(&ctx);
    while (size) {
        ULONG chunk = size > 0x40000000u ? 0x40000000u : (ULONG)size;
        xbox_XcSHAUpdate(&ctx, data, chunk);
        data += chunk;
        size -= chunk;
    }
    xbox_XcSHAFinal(&ctx, digest);
}

static void hex20(const unsigned char d[20], char out[41])
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 20; i++) {
        out[i * 2] = digits[d[i] >> 4];
        out[i * 2 + 1] = digits[d[i] & 15];
    }
    out[40] = 0;
}

int xbox_xbe_patches_apply_blob(const unsigned char *blob, size_t blob_size,
                                const uint8_t *xbe, size_t xbe_size,
                                uint8_t **out, size_t *out_size)
{
    const unsigned char *p, *end;
    uint32_t orig_size, new_size, runs, applied = 0, bytes = 0;
    unsigned char digest[20];
    char want[41], have[41];
    uint8_t *img;

    *out = NULL;
    *out_size = 0;
    if (!blob || !blob_size)
        return 0;

    if (blob_size < 60 || memcmp(blob, "XBEP", 4) != 0 || rd32(blob + 4) != 1) {
        fprintf(stderr, "[XBE-PATCH] the patch table linked into this build is "
                        "not one this runtime reads (format %u); lift again\n",
                blob_size >= 8 ? rd32(blob + 4) : 0u);
        return -1;
    }
    orig_size = rd32(blob + 8);
    new_size = rd32(blob + 12);
    runs = rd32(blob + 56);

    /* The file the patches were made from, and nothing else: the lifted code
     * expects the image they produce, and another release (or a different
     * dump) would put the patched bytes in the wrong places. */
    sha1_of(xbe, xbe_size, digest);
    if (xbe_size != orig_size || memcmp(digest, blob + 16, 20) != 0) {
        hex20(blob + 16, want);
        hex20(digest, have);
        fprintf(stderr,
                "[XBE-PATCH] this build was lifted from a patched default.xbe made "
                "from SHA-1 %s (%u bytes); the default.xbe found is %s (%zu bytes).\n"
                "[XBE-PATCH] The lifted code expects the patched image, so it "
                "cannot run on this file. Use the game files it was lifted from, "
                "or lift again from these.\n",
                want, orig_size, have, xbe_size);
        return -1;
    }
    if (new_size < orig_size) {
        fprintf(stderr, "[XBE-PATCH] malformed patch table (patched image smaller "
                        "than the original)\n");
        return -1;
    }

    img = (uint8_t *)calloc(1, new_size);
    if (!img) {
        fprintf(stderr, "[XBE-PATCH] out of memory for a %u-byte image\n", new_size);
        return -1;
    }
    memcpy(img, xbe, orig_size);

    p = blob + 60;
    end = blob + blob_size;
    for (uint32_t i = 0; i < runs; i++) {
        uint32_t off, len;
        if ((size_t)(end - p) < 8)
            break;
        off = rd32(p);
        len = rd32(p + 4);
        p += 8;
        if ((size_t)(end - p) < len || (uint64_t)off + len > new_size)
            break;
        memcpy(img + off, p, len);
        p += len;
        applied++;
        bytes += len;
    }

    sha1_of(img, new_size, digest);
    if (applied != runs || p != end || memcmp(digest, blob + 36, 20) != 0) {
        hex20(blob + 36, want);
        hex20(digest, have);
        fprintf(stderr, "[XBE-PATCH] the patch table does not produce the image "
                        "that was lifted (%u of %u runs; SHA-1 %s, expected %s); "
                        "lift again\n", applied, runs, have, want);
        free(img);
        return -1;
    }

    fprintf(stderr, "[XBE-PATCH] default.xbe patched as lifted: %u run(s), %u "
                    "byte(s), %zu -> %u bytes (SHA-1 %s)\n",
            runs, bytes, xbe_size, new_size, (hex20(digest, have), have));
    *out = img;
    *out_size = new_size;
    return 1;
}

int xbox_xbe_patches_apply(const uint8_t *xbe, size_t xbe_size,
                           uint8_t **out, size_t *out_size)
{
    unsigned int size = 0;
    const unsigned char *blob = recomp_xbe_patches(&size);
    return xbox_xbe_patches_apply_blob(blob, size, xbe, xbe_size, out, out_size);
}
