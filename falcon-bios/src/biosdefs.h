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
 * Still 0x7E00, and staying there is a decision rather than inertia. Stage 2
 * holds both 16-bit and 64-bit code, and the two disagree about what an
 * address means: real mode reaches it as an offset inside a 64 KB segment,
 * long mode as a linear address. Linking at 0x7E00 with a segment of zero is
 * the one arrangement where both readings are the same number, and it lasts
 * as long as stage 2 ends below 0xFFFF.
 *
 * What that costs is a ceiling of about 33 KB, which is why the page tables
 * and the bounce buffer moved out of the way rather than stage 2 moving up.
 * When it is no longer enough the answer is a third stage loaded above a
 * megabyte and entered after paging, not a bigger stage 2. */
#define STAGE2_LOAD     0x7E00

/* The sector stage 2 starts at, and how many it occupies.
 *
 * LBA 1 because stage 1 is LBA 0 and nothing else may live between them:
 * stage 1 reads a fixed range rather than looking anything up, which is what
 * keeps it inside 512 bytes. The count is the cap, not the size; the image
 * is padded to it so a short stage 2 still reads cleanly. */
#define STAGE2_LBA      1
/* 32 KB, which puts the end of stage 2 at 0xFE00: inside the segment it is
 * addressed through, and clear of the page tables now that they are at
 * 0x80000. A single INT 13h transfer may not cross a 64 KB boundary and this
 * one does not. */
#define STAGE2_SECTORS  64

/* The long-mode page tables, and why they are at fixed addresses.
 *
 * There is no allocator in real mode and nothing else is using this memory:
 * 0x80000 is above stage 2's 64 KB window, above the bounce buffer, and
 * still well under the 0x9FC00 where the extended BIOS data area starts on a
 * typical machine. Three pages is all long mode needs to start: one PML4,
 * one page-directory pointer table, and one page directory whose entries are
 * 2 MB pages.
 *
 * One gigabyte is mapped, identity. That covers every address real-mode
 * firmware can hand back and every address a payload is staged at before the
 * kernel takes over, and it costs exactly one page of directory entries. */
#define PML4_ADDR       0x80000
#define PDPT_ADDR       0x81000
#define PD_ADDR         0x82000
#define IDENTITY_MAP    0x40000000    /* 1 GiB */
#define PAGE_2M         0x200000

/* GDT selectors, in the order lmode.S lays the descriptors out. Shared
 * because the far jump that activates long mode names one of them and the C
 * that eventually runs there will name the other. */
#define SEL_CODE64      0x08
#define SEL_DATA64      0x10
/* The descriptors the thunk needs on the way down to real mode and back.
 * A 32-bit code segment (L=0, D=1) to leave long mode through, and a 16-bit
 * code and data pair whose 64 KB limits are what real mode expects to find
 * cached in the segment registers after CR0.PE is cleared. */
#define SEL_CODE32      0x18
#define SEL_CODE16      0x20
#define SEL_DATA16      0x28

/* COM1 and 115200 8N1. The divisor is 115200 / 115200 = 1.
 *
 * Serial rather than INT 10h for anything a test reads: the rig captures it
 * (falcon-efi/tools/run-qemu.sh --bios --headless), and a machine with no
 * usable video is exactly the machine this route exists for. */
#define SERIAL_PORT     0x3F8
#define SERIAL_DIVISOR  1

#endif /* FALCON_BIOSDEFS_H */
