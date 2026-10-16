/*
 * kernel_xbox.c - Xbox Identity & Hardware Stubs
 *
 * Provides Xbox-specific exported data (hardware info, kernel version,
 * keys, image filename) and section loading stubs.
 *
 * Most Xbox-specific hardware data is stubbed with plausible retail
 * values. Crypto keys are zeroed since we don't need Xbox Live or
 * EEPROM-based encryption on PC.
 */

#include "kernel.h"
#include "xbox_memory_layout.h"   /* xbox_EnvSwitch */
#include "recomp_config.h"
#include "xbox_nic.h"
#include "xbox_console_id.h"
#include <string.h>
/* getenv: without <stdlib.h> its pointer is truncated to int. */
#include <stdlib.h>

/* ============================================================================
 * Exported Data Objects
 *
 * These are global variables exported by the Xbox kernel at known ordinals.
 * Game code accesses them directly via the thunk table.
 * ============================================================================ */

/* Hardware info - default report is a standard retail Xbox.
 * A target may override via xbox_kernel_set_version() or by patching the
 * global directly; no title-specific values are baked in. */
XBOX_HARDWARE_INFO xbox_HardwareInfo = {
    .Flags       = 0x00000020,  /* Retail Xbox */
    .GpuRevision = 0xD2,       /* NV2A D2 revision */
    .McpRevision = 0xD4,       /* MCPX D4 revision */
    .Reserved    = {0, 0}
};

/* Kernel version - default is a stock retail value. Titles built against a
 * different XDK can report their own through xbox_kernel_set_version(). */
XBOX_KRNL_VERSION xbox_KrnlVersion = {
    .Major = 1,
    .Minor = 0,
    .Build = 5849,
    .Qfe   = 1
};

/* Override the kernel version reported to game code (ordinal 324).
 * Match the XDK revision the target title was built with if it checks it. */
void xbox_kernel_set_version(USHORT major, USHORT minor, USHORT build, USHORT qfe)
{
    xbox_KrnlVersion.Major = major;
    xbox_KrnlVersion.Minor = minor;
    xbox_KrnlVersion.Build = build;
    xbox_KrnlVersion.Qfe   = qfe;
}

/* Crypto keys - zeroed, not needed for PC operation.
 * These are unique per-console on real hardware. */
UCHAR xbox_EEPROMKey[16]              = {0};
UCHAR xbox_HDKey[16]                  = {0};
UCHAR xbox_SignatureKey[16]           = {0};
UCHAR xbox_LANKey[16]                 = {0};
UCHAR xbox_AlternateSignatureKeys[16][16] = {{0}};

/* Public key data for Xbox Live signature verification - not needed */
UCHAR xbox_XePublicKeyData[284] = {0};

/* Image filename - the XBE path as seen by the kernel */
static char g_image_filename[] = "\\Device\\CdRom0\\default.xbe";
XBOX_ANSI_STRING xbox_XeImageFileName = {
    .Length        = sizeof(g_image_filename) - 1,
    .MaximumLength = sizeof(g_image_filename),
    .Buffer        = g_image_filename
};

/* Launch data page - used for title-to-title launches (e.g., Xbox Dashboard → game).
 * Allocated dynamically and zeroed for normal game boot. */
static XBOX_LAUNCH_DATA_PAGE g_launch_data_page = {0};
XBOX_LAUNCH_DATA_PAGE* xbox_LaunchDataPage = &g_launch_data_page;

/* ============================================================================
 * Section Loading
 *
 * XeLoadSection/XeUnloadSection manage on-demand loading of XBE sections.
 * On Xbox, some sections are demand-paged from disc. In our recompilation,
 * the entire executable is loaded into memory, so these are reference-counting
 * no-ops.
 * ============================================================================ */

NTSTATUS __stdcall xbox_XeLoadSection(PXBE_SECTION_HEADER Section)
{
    if (!Section)
        return STATUS_INVALID_PARAMETER;

    InterlockedIncrement(&Section->SectionReferenceCount);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_XBOX,
        "XeLoadSection: '%s' at %p (size=%u, refcount=%d)",
        Section->SectionName ? Section->SectionName : "<null>",
        Section->VirtualAddress, Section->VirtualSize,
        Section->SectionReferenceCount);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_XeUnloadSection(PXBE_SECTION_HEADER Section)
{
    if (!Section)
        return STATUS_INVALID_PARAMETER;

    LONG new_count = InterlockedDecrement(&Section->SectionReferenceCount);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_XBOX,
        "XeUnloadSection: '%s' (refcount=%d)",
        Section->SectionName ? Section->SectionName : "<null>",
        new_count);

    return STATUS_SUCCESS;
}

/* ============================================================================
 * EEPROM / Non-Volatile Settings
 *
 * ExQueryNonVolatileSetting / ExSaveNonVolatileSetting read and write EEPROM
 * settings. On real hardware these are stored in the 256-byte EEPROM on the
 * SMBus. For recompilation, we return sensible defaults:
 *   - Region: North America
 *   - Video: NTSC, widescreen+HDTV enabled
 *   - Language: English
 *   - Audio: Stereo, Dolby Digital
 *   - DVD Region: Region 1 (North America)
 * ============================================================================ */

/* Game region the running title's certificate allows.
 *
 * Set from the mapped XBE during layout init. Zero means there was no XBE to
 * read -- a synthetic test image, or the static fallback path -- and the
 * factory answers below then pick North America rather than refusing.
 */
static uint32_t s_xbe_game_region;

void xbox_kernel_set_xbe_game_region(uint32_t region)
{
    s_xbe_game_region = region;
}

uint32_t xbox_kernel_get_xbe_game_region(void)
{
    return s_xbe_game_region;
}

/* The console region to report, as the narrowest one this disc allows.
 *
 * A title's check is `cert_region & console_region`, so any single bit the
 * certificate sets will pass. Preferring NA keeps NTSC-M as the video
 * standard for the region-free discs that set all three, which is what a
 * North American console would report; RECOMP_XBOX_REGION overrides it for a
 * PAL or Japanese disc whose title also keys language or timing off the
 * region it sees.
 */
static uint32_t console_game_region(void)
{
    const char *env = getenv("RECOMP_XBOX_REGION");
    uint32_t allowed = s_xbe_game_region;

    if (env) {
        if (env[0] == 'j' || env[0] == 'J') return XC_GAME_REGION_JAPAN;
        if (env[0] == 'r' || env[0] == 'R'
         || env[0] == 'p' || env[0] == 'P') return XC_GAME_REGION_RESTOFWORLD;
        return XC_GAME_REGION_NA;
    }

    if (allowed & XC_GAME_REGION_NA)          return XC_GAME_REGION_NA;
    if (allowed & XC_GAME_REGION_RESTOFWORLD) return XC_GAME_REGION_RESTOFWORLD;
    if (allowed & XC_GAME_REGION_JAPAN)       return XC_GAME_REGION_JAPAN;
    return XC_GAME_REGION_NA;
}

static uint32_t console_av_region(void)
{
    switch (console_game_region()) {
    case XC_GAME_REGION_JAPAN:       return XC_AV_STANDARD_NTSC_J;
    case XC_GAME_REGION_RESTOFWORLD: return XC_AV_STANDARD_PAL_I;
    default:                         return XC_AV_STANDARD_NTSC_M;
    }
}

/* PAL60: whether a PAL console says it can show 60 Hz (the dashboard's
 * "PAL 60" setting, XC_VIDEO_FLAGS_60Hz). A title that supports it picks a
 * 60 Hz mode and runs its own 60 Hz timing; told 50, it asks for 50 Hz and
 * paces itself for 50 while the host shows 60. Gladius (sub_001E0E40) reads
 * exactly this bit to choose 60 or 50. On by default, since a PAL60 console
 * is the one that matches how the host presents; RECOMP_PAL60=0 (pal60 = 0)
 * is a 50 Hz-only PAL console. Nothing changes on an NTSC console. */
int xbox_kernel_console_pal60(void)
{
    return console_av_region() == XC_AV_STANDARD_PAL_I &&
           recomp_config_bool("RECOMP_PAL60", "pal60", 1);
}

/* The same answer for AvSendTVEncoderOption (kernel_hal.c), which reports the
 * video standard and refresh in the same bits: a title reads its standard from
 * one and D3D validates the mode against the other, so they must agree. */
uint32_t xbox_kernel_console_av_standard(void)
{
    return console_av_region();
}

NTSTATUS __stdcall xbox_ExQueryNonVolatileSetting(
    ULONG ValueIndex, PULONG Type, PVOID Value, ULONG ValueLength, PULONG ResultLength)
{
    if (!Value)
        return STATUS_INVALID_PARAMETER;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_XBOX,
        "ExQueryNonVolatileSetting: index=0x%02X len=%u", ValueIndex, ValueLength);

    switch (ValueIndex) {
    case XC_LANGUAGE:
        /* English = 1 */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 1;
            if (Type) *Type = 4; /* REG_DWORD */
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_VIDEO:
        /* A 4:3 NTSC console by default, which is what every title has
         * actually been told so far: the flags used to be written into the
         * low half-word, where XAPI's shift never finds them, so the
         * widescreen and HDTV this claimed to advertise both read as
         * absent. Keeping the default at none preserves that behaviour
         * now the encoding is right, instead of switching titles into
         * 16:9 and HDTV code paths that have never run here.
         *
         * RECOMP_WIDESCREEN turns the widescreen bit on, and a title with
         * a 16:9 mode of its own then uses it. The host's output shape
         * reads the same variable (src/d3d/d3d8_display.c): a title told
         * it is widescreen while the host presents 4:3 looks worse than
         * one told the truth, so the two must not disagree. */
        if (ValueLength >= sizeof(ULONG)) {
            ULONG flags = 0;

            if (recomp_config_bool("RECOMP_WIDESCREEN", "widescreen", 0))
                flags |= XC_VIDEO_FLAGS_WIDESCREEN;
            if (xbox_kernel_console_pal60())
                flags |= XC_VIDEO_FLAGS_60Hz;
            *(PULONG)Value = XC_VIDEO_RAW(flags);
            if (Type) *Type = 4; /* REG_DWORD */
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_AUDIO:
        /* Two-channel, no encoders.
         *
         * This used to answer 0x00010001, described as "stereo + Dolby
         * Digital". Stereo is 0 in this field and 1 is mono, so it actually
         * said mono -- and then advertised an AC3 encoder on top, which is a
         * combination no console reports.
         *
         * The encoder bits are the part that matters. Advertising AC3 tells a
         * title the digital output can carry an encoded stream, so it
         * configures its mixer for one; Burnout 2 goes on to load
         * data/b2distfx.bin, an effects image for the APU's DSP, and this
         * runtime has no DSP to download it to. Claiming a capability that is
         * not implemented is worse than claiming none.
         */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = XC_AUDIO_FLAGS_STEREO;
            if (Type) *Type = 4; /* REG_DWORD */
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_P_CONTROL_GAMES:
    case XC_P_CONTROL_MOVIES:
        /* Unrestricted. Titles compare their XBE certificate's rating against
         * this and refuse to run (Halo boots to the dashboard) if the console
         * is configured more strictly, so 0 means "no restriction". */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 0;
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_DVD_REGION:
        /* Region 1 (North America) */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 1;
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_MISC:
        /* Misc flags: 0 = no special flags */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 0;
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_TIMEZONE_BIAS:
        /* UTC-5 (Eastern Time) in minutes: -300 */
        if (ValueLength >= sizeof(LONG)) {
            *(PLONG)Value = -300;
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(LONG);
        }
        break;

    /* ---- the factory block ----
     *
     * Burnout 2 asks for both region entries during init and got zeroed
     * memory from the default arm below, because the switch stopped at the
     * user settings. Zero is the one answer a region check can never accept.
     */
    case XC_FACTORY_GAME_REGION:
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = console_game_region();
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_FACTORY_AV_REGION:
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = console_av_region();
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_FACTORY_SERIAL_NUMBER:
        /* Twelve ASCII digits, no terminator. Real consoles encode the plant
         * and week here. The player's own console's when there is one
         * (xbox_console_id.c): Xbox Live names the machine account after it. */
        if (ValueLength >= 12) {
            const XboxConsoleId *id = xbox_ConsoleId();
            memcpy(Value, id ? id->serial : "100000000000", 12);
            if (Type) *Type = 3;              /* REG_BINARY */
            if (ResultLength) *ResultLength = 12;
        }
        break;

    case XC_FACTORY_ETHERNET_ADDR: {
        /* Six bytes, in Microsoft's Xbox OUI so a title that sanity-checks
         * the prefix is satisfied. System Link needs it stable and different
         * on every console in a game, so it is chosen once per save folder
         * (xbox_nic.c) rather than being the one address every install had. */
        uint8_t mac[6];
        xbox_NicMacAddress(mac);
        if (ValueLength >= sizeof mac) {
            memcpy(Value, mac, sizeof mac);
            if (Type) *Type = 3;
            if (ResultLength) *ResultLength = (ULONG)sizeof mac;
        }
        break;
    }

    case XC_FACTORY_ONLINE_KEY:
        /* Sixteen bytes, per-console, and only Xbox Live uses it: the
         * machine account is created with it. The player's own console's
         * when there is one, else zeros. */
        if (ValueLength >= 16) {
            const XboxConsoleId *id = xbox_ConsoleId();
            if (id)
                memcpy(Value, id->online_key, 16);
            else
                memset(Value, 0, 16);
            if (Type) *Type = 3;
            if (ResultLength) *ResultLength = 16;
        }
        break;

    default:
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_XBOX,
            "ExQueryNonVolatileSetting: unhandled index 0x%02X", ValueIndex);
        memset(Value, 0, ValueLength);
        if (Type) *Type = 4;
        if (ResultLength) *ResultLength = ValueLength;
        break;
    }

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_ExSaveNonVolatileSetting(
    ULONG ValueIndex, ULONG Type, PVOID Value, ULONG ValueLength)
{
    (void)Type;
    (void)Value;
    (void)ValueLength;

    xbox_log(XBOX_LOG_INFO, XBOX_LOG_XBOX,
        "ExSaveNonVolatileSetting: index=0x%02X len=%u (ignored - read-only on PC)",
        ValueIndex, ValueLength);

    /* Accept writes silently but don't persist them */
    return STATUS_SUCCESS;
}

/* ============================================================================
 * Network / PHY
 *
 * PhyGetLinkState reports Ethernet link status. Xbox had a built-in 100Mbit
 * NIC. For PC, we report link-up since we'll handle networking differently.
 * ============================================================================ */

ULONG __stdcall xbox_PhyGetLinkState(BOOLEAN Verify)
{
    /* RECOMP_ETHERNET_LINK=0: no cable. There is no network card behind
     * this, so "link up" sends XNet to DHCP with nobody to answer: Rainbow
     * Six 3 polls XNetGetTitleXnAddr in a loop at start-up and sat in DHCP
     * retries for 25 to 100 s before XNet gave up. With no link XNet marks
     * the address "troubleshoot" at once and the title moves on, as a
     * console with nothing plugged in does. Off by default, because a title
     * with a System Link menu says "no cable" rather than searching.
     *
     * With the network card on (System Link, xbox_nic.c) there is a cable,
     * whatever the variable says: a title that turns the link off for the
     * no-card case, as Rainbow Six 3 does, still finds it. */
    static int link = -1;
    (void)Verify;
    if (link < 0)
        link = xbox_NicCardOn() || xbox_EnvSwitch("RECOMP_ETHERNET_LINK", 1);
    /* Link up, 100 Mbps, full duplex */
    return link ? 0x01 : 0x00; /* XNET_ETHERNET_LINK_ACTIVE or nothing */
}

NTSTATUS __stdcall xbox_PhyInitialize(BOOLEAN ForceReset, PVOID Param2)
{
    (void)ForceReset;
    (void)Param2;
    return STATUS_SUCCESS;
}
