/*
 * The default for a title lifted from an unpatched XBE: no patches.
 *
 * A title lifted with an XBE patch file links a generated
 * recomp_xbe_patches.c (tools.recomp) whose definition wins over this weak
 * one. Kept in a file of its own so the default is never inlined into its
 * caller. MSVC has no weak symbols; xbe_patches.c falls back by
 * /alternatename there, and this file is empty.
 */
#include "xbe_patches.h"

#if !defined(_MSC_VER)
__attribute__((weak)) const unsigned char *recomp_xbe_patches(unsigned int *size)
{
    *size = 0;
    return NULL;
}
#else
typedef int xbe_patches_none_is_empty_on_msvc;
#endif
