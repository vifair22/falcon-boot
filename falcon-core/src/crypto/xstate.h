/* falcon-boot: the AVX register state, borrowed for an unlock and given
 * back.
 *
 * Purpose. AVX registers exist only once system software has turned their
 * state on: CR4.OSXSAVE, then XCR0's SSE and AVX bits through XSETBV. An
 * operating system does that for its programs, so the host tools find AVX2
 * on and argon2id's AVX2 block function runs there by itself. Neither
 * loader has an operating system under it. falcon-bios runs in protected
 * mode with CR4 as pmode.S left it, OSXSAVE clear; a UEFI firmware may or
 * may not have set it. So each loader borrows the state around the one
 * thing that wants it, the keyslot derivation, and the probe
 * (crypto_simd_level) sees AVX2 only while it is borrowed.
 *
 * Reasoning. Borrowed and given back, never turned on for good. What runs
 * after the loader inherits CR4 and XCR0: a kernel sets both itself, but a
 * chained DOS, Windows 9x or NT boot sector does not, and with OSXSAVE left
 * set, CPUID would tell its programs the system saves the AVX registers
 * across a task switch when it does not. The firmware, likewise, gets back
 * exactly the state it had. Before the state is given back every ymm
 * register is cleared, which is also the wipe of what the derivation left
 * in them.
 *
 * A processor without XSAVE and AVX, or whose CPUID does not offer the
 * SSE and AVX state components, is left alone, and the derivation runs at
 * the highest level it has without them.
 *
 * Ring 0 only: CR4 and XSETBV fault anywhere else. The loaders link this
 * (CORE_XSTATE); no host build does.
 *
 * Interactions. falcon-efi/src/pos/pos.c and falcon-bios/src/pos/pos.c
 * call it around luks_unlock; crypto.c's crypto_simd_level reads what it
 * sets.
 */

#ifndef FALCON_CRYPTO_XSTATE_H
#define FALCON_CRYPTO_XSTATE_H

#include <stdint.h>

typedef struct {
    uintptr_t cr4;      /* CR4 before the borrow */
    uint64_t  xcr0;     /* XCR0 to restore */
    int       changed;  /* 0: nothing was borrowed, nothing to give back */
} xstate_saved;

/* Turns the SSE and AVX state on, when the processor has it and it is not
 * on already, and records what to give back in *s. */
void xstate_borrow(xstate_saved *s);

/* Clears every ymm register and puts CR4 and XCR0 back as *s recorded.
 * Nothing when the borrow changed nothing. */
void xstate_return(const xstate_saved *s);

#endif /* FALCON_CRYPTO_XSTATE_H */
