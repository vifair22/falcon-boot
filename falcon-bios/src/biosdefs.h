/* falcon-bios: the constants both the assembly and the C side need
 * (docs/notes/bios-route.md Section 5.2).
 *
 * This file is the whole reason the assembly is gas `.S` rather than NASM.
 * A `.S` is preprocessed, so a load address written here reaches the linker
 * script, the boot sector and the C that will eventually run at it, and there
 * is one place to change it. NASM would mean writing every one of these down
 * twice and finding out about a disagreement at run time, in real mode, with
 * no debugger.
 *
 * Nothing here is a guess: the addresses are the ones the PC's real-mode
 * memory map leaves usable, and they are commented with why.
 */

#ifndef FALCON_BIOSDEFS_H
#define FALCON_BIOSDEFS_H

/* Where the BIOS puts the boot sector. Fixed by every PC BIOS since the
 * first one; not ours to choose. */
#define STAGE1_LOAD     0x7C00

/* Where stage 2 is loaded, immediately above stage 1.
 *
 * The usable low memory on a PC is roughly 0x00500 to 0x9FBFF, with the
 * interrupt vector table and the BIOS data area below it and the extended
 * BIOS data area at the top. 0x7E00 upward is the conventional place for a
 * second stage because it is contiguous with the sector the BIOS already
 * loaded, and it leaves the stack room below 0x7C00. */
#define STAGE2_LOAD     0x7E00

/* The sector stage 2 starts at, and how many it occupies.
 *
 * LBA 1 because stage 1 is LBA 0 and nothing else may live between them:
 * stage 1 reads a fixed range rather than looking anything up, which is what
 * keeps it inside 512 bytes. The count is the cap, not the size; the image
 * is padded to it so a short stage 2 still reads cleanly. */
#define STAGE2_LBA      1
#define STAGE2_SECTORS  16

/* COM1 and 115200 8N1. The divisor is 115200 / 115200 = 1.
 *
 * Serial rather than INT 10h for anything a test reads: the rig captures it
 * (falcon-efi/tools/run-qemu.sh --bios --headless), and a machine with no
 * usable video is exactly the machine this route exists for. */
#define SERIAL_PORT     0x3F8
#define SERIAL_DIVISOR  1

#endif /* FALCON_BIOSDEFS_H */
