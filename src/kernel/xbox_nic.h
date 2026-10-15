/**
 * The console's network card: its Ethernet address, and its registers.
 *
 * See xbox_nic.c for what is modelled so far and how to use it.
 */
#ifndef XBOX_NIC_H
#define XBOX_NIC_H

#include <stdint.h>

/* The card's register page. The nForce MAC answers on 1 KB from here; the
 * whole page is trapped, because protection is per page. */
#define XBOX_NIC_BASE 0xFEF00000u
#define XBOX_NIC_SIZE 0x00001000u

/* This console's Ethernet address, which XNet reads through
 * ExQueryNonVolatileSetting(XC_FACTORY_ETHERNET_ADDR). Chosen once per save
 * folder and kept there; RECOMP_SYSLINK_MAC overrides it. */
void xbox_NicMacAddress(uint8_t out[6]);

/* XboxLANKey: the player's own, from keys.ini or RECOMP_XBOX_LAN_KEY, else
 * zeros (which only other recompiled builds share). See xbox_nic.c. */
void xbox_NetLanKey(uint8_t out[16]);

/* Trap the register page if anything asks for it (RECOMP_SYSLINK for the
 * card, RECOMP_NIC_TRACE for the log). host is the page's host address. Call
 * once, after the MCPX aperture is mapped. The card registers its own fault
 * range (recomp_fault_add_range), on Windows and POSIX alike. */
void xbox_NicInit(void *host);

/* Whether the card is on (and its page trapped): a cable is plugged in, so
 * PhyGetLinkState reports a link whatever RECOMP_ETHERNET_LINK says. */
int  xbox_NicCardOn(void);

/* From the kernel's timer thread: received frames into the receive ring,
 * and the summary every five seconds. */
void xbox_NicTick(void);

/* Whether the card's interrupt line is asserted ((IrqStatus & IrqMask) != 0),
 * for the kernel to raise the ISR on bus level 4. */
int  xbox_NicIrqPending(void);

#endif /* XBOX_NIC_H */
