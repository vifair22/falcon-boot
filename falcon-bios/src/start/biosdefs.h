/* falcon-bios: the constants both the assembly and the C side need.
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
 * holds both 16-bit and 32-bit code, and the two disagree about what an
 * address means: real mode reaches it as an offset inside a 64 KB segment,
 * protected mode as a linear address. Linking at 0x7E00 with a segment of
 * zero is the one arrangement where both readings are the same number, and
 * it lasts as long as stage 2 ends below 0xFFFF.
 *
 * What that costs is a ceiling of about 33 KB, which is why the bounce
 * buffer moved out of the way rather than stage 2 moving up. When it is no
 * longer enough the answer is a third stage loaded above a megabyte, not a
 * bigger stage 2. */
#define STAGE2_LOAD     0x7E00

/* Stage 2's place on a GPT disk, and the ceiling on its length.
 *
 * 34, and the number is the GPT's, not ours: LBA 0 is the protective MBR
 * (stage 1), LBA 1 the GPT header and LBA 2..33 the 128-entry array, and
 * 34 is the first sector the header declares usable, in front of the first
 * 2048-aligned partition (on a stick, inside the FALCON-BIOS EF02
 * partition). An MBR disk's is STAGE2_LBA_MBR below.
 *
 * Stage 1 does not use either number: it reads what the installer wrote
 * into it (STAGE1_S2_LBA_OFF), so this is what the installers and the rigs
 * write, and what jump's floppy stage 1 (boot_fd.S) reads, since the floppy
 * keeps its own fixed layout. STAGE2_SECTORS is the reservation's upper
 * bound only, not a read length: 32 KB puts the end of stage 2 at 0xFE00,
 * inside the segment it is addressed through, so no single INT 13h transfer
 * into it crosses a 64 KB boundary. */
#define STAGE2_LBA      34
#define STAGE2_SECTORS  64
/* What an old MBR disk leaves stage 2: LBA 1 to 62, in front of a first
 * partition at 63 (the A21m's Windows 2000 partition). The build refuses a
 * stage 2 longer than this, so one stage 2 fits every disk. */
#define STAGE2_MBR_MAX_SECTORS 62
/* Where the floppy boot sector (boot_fd.S) keeps the count of stage 2's
 * sectors it reads, which the build writes after mformat: the two bytes
 * before the boot signature. */
#define FD_S2_SECTORS_OFF 508
/* Jump's floppy keeps the layout it had: stage 2 at STAGE2_LBA of the
 * volume and stage 3 at this sector, both inside the FAT12 volume's
 * reserved sectors. Its identity record names this sector as stage 3's, as
 * every boot partition's record names its own; this is what the floppy's
 * recipe writes (apps/jump/app.mk). */
#define FD_STAGE3_SECTOR 98

/* The single BIOS layout (docs/DESIGN.md Section 3.3). Everything builds
 * against these names; nothing else writes the numbers.
 *
 * Stage 1 does not know where stage 2 is: the installer writes stage 2's
 * first LBA (LE32) and its real length in sectors (LE16) into stage 1 at
 * these offsets, inside the 440 bytes of boot code, which ends before them
 * (boot.S; build.mk checks the symbols). STAGE2_LBA above is the GPT place
 * (LBA 34); an MBR disk's is LBA 1. */
#define STAGE1_S2_LBA_OFF      0x1B2
#define STAGE1_S2_SECTORS_OFF  0x1B6
#define STAGE2_LBA_MBR         1

/* Inside the `boot` partition (FAT; a stick's ESP, a station partition),
 * counted in sectors from its start. FAT32's own reserved sectors are 0
 * (boot), 1 (FSInfo), 6 and 7 (backups); the marker takes 8 and stage 3
 * starts at 32. The identity record carries both, and the loaders read them
 * from the record; these are what the installers write. */
#define BOOTPART_MARKER_SECTOR 8
#define BOOTPART_STAGE3_SECTOR 32

/* "BOS2", at GATE_MAGIC: what stage 1 checks before it jumps, so sectors
 * that are not stage 2 (a stick whose gap was written over, a disk with
 * 4 KiB sectors that put LBA 34 somewhere else) stop with a message rather
 * than running whatever was read. */
#define STAGE2_MAGIC    0x32534F42

/* Where the loader runs: flat 32-bit protected mode, paging off, so a
 * linear address is a physical one.
 *
 * The whole loader is i586 code, which is what lets one image boot the
 * oldest machines this route exists for as well as every newer one. What a newer processor has, it gets by asking rather than by a second
 * build: stage 2 turns the SSE state on when CPUID says there is SSE, and
 * falcon-core's AES path uses AES-NI when CPUID says there is AES-NI. Long
 * mode buys a BIOS loader nothing: every object it places goes below 4 GiB,
 * which is where the boot protocol and the BIOS want them anyway.
 *
 * ADDR_END is the end of what the loader can address, and so the ceiling on
 * where anything is placed. C only: the assembler has no 64-bit constants. */
#define ADDR_END        0x100000000ull

/* GDT selectors, in the order pmode.S lays the descriptors out. A flat
 * 32-bit code and data pair for the loader, and a 16-bit code and data pair
 * whose 64 KB limits are what real mode expects to find cached in the
 * segment registers when the thunk clears CR0.PE. */
#define SEL_CODE32      0x08
#define SEL_DATA32      0x10
#define SEL_CODE16      0x18
#define SEL_DATA16      0x20

/* ---- the gate ----
 *
 * The fixed addresses stage 3 reaches the BIOS through. Stage 3 is linked
 * separately and above a megabyte; it shares no symbol with the assembly and
 * cannot, because the thunk drops to real mode and real mode has twenty
 * address bits. So the thunk stays in stage 2 and stage 2 publishes a way in
 * at an address both halves can write down.
 *
 * The head of stage 2 is that address. It is fixed already - stage 1 loads
 * stage 2 at STAGE2_LOAD and jumps there - so the gate costs nothing to
 * place, and the entry stage 1 jumps to is a jump over it. Anywhere else
 * would be a second address to keep true, and the one at 0x7400 that this
 * was first written at was worse than that: below stage 2's load address, so
 * it could not be in stage 2's image at all.
 */
#define GATE_ADDR       STAGE2_LOAD
#define GATE_MAGIC      (GATE_ADDR + 0x04)   /* STAGE2_MAGIC, for stage 1 */
#define GATE_THUNK      (GATE_ADDR + 0x08)   /* jmp thunk_bios */
#define GATE_BOOT16     (GATE_ADDR + 0x10)   /* jmp boot16, and never back */
#define GATE_BOOTPART   (GATE_ADDR + 0x18)   /* LE64: the boot partition's */
                                             /* first LBA; 0 on a floppy */
#define GATE_REGS       (GATE_ADDR + 0x20)   /* the register block */
#define GATE_VECTOR     (GATE_ADDR + 0x40)   /* the interrupt to raise */
#define GATE_DRIVE      (GATE_ADDR + 0x41)   /* the drive to hand over in DL */
#define GATE_MARKER     (GATE_ADDR + 0x42)   /* LE16: the next-boot marker's */
                                             /* sector in it; 0 for none */
#define GATE_SI         (GATE_ADDR + 0x44)   /* LE16: SI at boot16's jump, */
                                             /* DS 0; 0 but for a partition */
                                             /* chain (mbr/chain.c) */
#define GATE_TARGET     (GATE_ADDR + 0x48)   /* where boot16 lands: off, seg */
#define GATE_S2LEN      (GATE_ADDR + 0x4C)   /* stage 2's length, CRC-32 after */

/* Where a structure the BIOS itself reads or writes has to live.
 *
 * The bounce buffer covers data, but a disk address packet and a drive
 * parameter block are addressed the same way and the same twenty bits apply
 * to them. A static in stage 3 is above a megabyte, so real mode truncates
 * its address to sixteen bits and the BIOS reads whatever is at the result.
 * That failure is quiet: INT 13h returns carry, and the loader reports a
 * disk that will not read.
 *
 * In the low 64 KB, not merely below a megabyte, because the thunk hands the
 * BIOS a pointer as DS:SI with DS zero: anything above 0xFFFF is truncated
 * to its bottom sixteen bits and the BIOS reads a different address entirely.
 * 0x1000 is above the interrupt vector table and the BIOS data area and
 * below everything this loader places. */
#define BIOS_SCRATCH    0x00001000
#define DAP_ADDR        (BIOS_SCRATCH + 0x00)   /* 16 bytes */
#define DRIVEPARM_ADDR  (BIOS_SCRATCH + 0x40)   /* 30 bytes */
#define E820_ADDR       (BIOS_SCRATCH + 0x80)   /* 24 bytes, one entry */

/* ---- where a Linux payload goes (DESIGN Section 11.3.6) ----
 *
 * The zero page and the command line at the addresses every BIOS loader has
 * used for them since LILO, which is the useful property: a kernel's
 * decompressor is known not to land on them. The kernel itself at 16 MB,
 * which is above everything this loader occupies and 2 MB aligned, as a
 * relocatable kernel's preferred address must be. The initrd is not fixed:
 * it is placed from the memory map, as high as the kernel will accept it,
 * because it is the one object whose size is not known until it is read. */
#define ZEROPAGE_ADDR   0x00010000
#define CMDLINE_ADDR    0x00020000
#define KERNEL_ADDR     0x01000000

/* The handoff's last few instructions and the GDT the kernel is entered
 * with, copied below a megabyte just before the jump (linux.c). A kernel
 * that is not relocatable runs at 1 MB, which is where stage 3 is; it is
 * staged higher and moved down by code that has to be somewhere the move
 * does not reach. Between the scratch block and the real-mode stack, which
 * grows down from 0x7C00 and is a few hundred bytes deep. */
#define LINUX_STUB_ADDR 0x00003000
#define LINUX_GDT_ADDR  0x00003100

#define STAGE3_LOAD     0x100000

/* The page the other processors start in for an unlock (mp/aptramp.S,
 * mp/mp.c), copied there before each start. A startup IPI names a 4 KB
 * page below a megabyte by its number and starts the processor in real
 * mode at its first byte. Between the Linux stub and the real-mode stack,
 * which grows down from 0x7000 only a few hundred bytes, and used only
 * while the keyslots run: nothing else lives in it. Data at fixed offsets,
 * filled by mp.c, then the IDT the processors run under and the code; the
 * offsets are the .org's in the assembly. */
#define APT_ADDR        0x00005000
#define APT_VECTOR      (APT_ADDR >> 12)
#define APT_OFF_GDTR    0x08     /* 6 bytes: the boot processor's GDTR */
#define APT_OFF_IDTR    0x10     /* 6 bytes: this page's IDT */
#define APT_OFF_ENTRY   0x18     /* dword: the C entry, mp_ap_main */
#define APT_OFF_STACKS  0x1C     /* dword: the stacks' base */
#define APT_OFF_STACKB  0x20     /* dword: bytes a stack */
#define APT_OFF_NSTACK  0x24     /* dword: stacks */
#define APT_OFF_TICKET  0x28     /* dword: processors arrived */
#define APT_OFF_IDT     0x40     /* 32 gates, every one the halt */
#define APT_IDT_GATES   32
#define APT_OFF_HALT    0x140    /* the halt every fault lands on */
#define APT_OFF_CODE    0x150
#define APT_BYTES       0x400

/* Stage 2's own memory once it is in protected mode, both borrowed from
 * stage 3's, which is not there yet.
 *
 * Its stack is stage 3's. Below 0x7C00 was room for reading one header, but
 * finding the boot partition reads a partition table, whose readers keep a
 * sector or two on the stack, and the thunk's real-mode stack starts at
 * 0x7000: a protected-mode frame below that is written over by the BIOS
 * call it is waiting on.
 *
 * The partition list and the sector buffer it searches with (load3.c) go at
 * GZ_STAGE_ADDR, which stage 2 is done with before the compressed stage 3 is
 * read there. Stage 2 has no .bss, so they cannot be statics. */
#define STAGE2_STACK    STAGE3_STACK
#define STAGE2_SCRATCH  GZ_STAGE_ADDR

/* Where the BIOS is asked to put what it reads, and take what it writes.
 *
 * Below one megabyte, so real mode can reach it; above stage 2's window; and
 * below the 0x9FC00 where the extended BIOS data area tends to start on a
 * real machine (stage2.S refuses an EBDA below 0x98000). 32 KB is enough
 * that a sequential read is not one interrupt per sector and small enough to
 * be sure it fits. disk.c reads through it and marker.c writes through it. */
#define BOUNCE_ADDR     0x90000
#define BOUNCE_BYTES    0x8000

/* The last track read from a drive with no INT 13h extensions (a floppy),
 * kept (disk.c): on a floppy every read call waits for its first sector to
 * come round, half a revolution on average, so the single sectors a FAT
 * walk reads cost a revolution each unless the track they sit on is already
 * here. Below the bounce buffer, inside one 64 KB page as floppy DMA needs,
 * and big enough for any track INT 13h can describe (63 sectors). */
#define TRACK_CACHE_ADDR  0x88000
#define TRACK_CACHE_BYTES 0x8000

/* Stage 3 as linked begins with a header rather than code: the magic, its
 * length, and the memory it takes with its .bss. STAGE3_HDR bytes. */
#define STAGE3_MAGIC    0x33534F42   /* "BOS3" */
#define STAGE3_HDR      16           /* magic, two lengths, room to grow */

/* Stage 3 as it is on the disk, for every app: its own header, then a gzip
 * member of the linked image, "BOS3" header and all. The header is the
 * magic, the file's length, the memory the inflated stage 3 takes with its
 * .bss, and the inflated length, so stage 2 reads one sector, learns how
 * much to read, and reads exactly that. It reads the member to
 * GZ_STAGE_ADDR and inflates it to STAGE3_LOAD, which stage 3 may fill up
 * to GZ_STAGE_ADDR; the member's CRC-32 is the check that the disk gave
 * back what was written. */
#define STAGE3_MAGIC_GZ 0x5A534F42   /* "BOSZ" */
#define GZ_STAGE_ADDR   0x00600000


/* ---- the Safe MBR Hook header (DESIGN Section 11.3.8) ----
 *
 * Windows 98's I/O supervisor walks the INT 13h chain at startup and
 * reads the 98 DDK's SafeMBRHook structure at each handler's entry: a
 * short jump over the header, `$INT13SF`, a vendor ID, the previous
 * handler, and flags whose bit 0 says the hook is safe to take over. Every
 * INT 13h entry a falcon-bios resident installs begins with it, flags 0,
 * and keeps the previous vector in its smhnext slot, so each block's
 * X_OFF_OLDINT13 is X_OFF_CODE + SMH_OFF_NEXT and the handler proper is at
 * X_OFF_CODE + SMH_BYTES. The other vectors a block hooks (INT 15h, 16h)
 * have no header. */
#define SMH_OFF_NEXT     19       /* smhnext inside the header */
#define SMH_BYTES        27       /* the header; the handler follows it */

#ifdef __ASSEMBLER__
/* The header, at a resident's INT 13h entry: `.org X_OFF_CODE`, this, the
 * handler. The .org inside is the check that smhnext is at X_OFF_OLDINT13,
 * which the C side writes and the handler chains through. */
.macro safe_mbr_hook oldint13
	jmp     1f
	nop
	.ascii  "$INT13SF"
	.ascii  "FALCON  "
	.org    \oldint13
	.long   0                    /* smhnext: the previous handler, installed */
	.long   0                    /* smhflgs */
1:
.endm
#endif

/* ---- the virtual CD's resident (DESIGN Section 11.3.4) ----
 *
 * One paragraph-aligned block in conventional memory, taken by shrinking
 * what the BIOS data area says the machine has, which is how MEMDISK and
 * every other loader of this shape keeps a chainloaded bootloader from
 * landing on it.
 *
 * The block is data first and code second, at fixed offsets, so the handler
 * can reach its own state with a CS override and does not care what segment
 * it was copied to. The offsets are here, `.org` places them in vcd.S, and
 * the build checks the two agree.
 */
#define VCD_OFF_DRIVE    0x0004   /* byte: the drive we answer for */
#define VCD_OFF_PHYS     0x0005   /* byte: the drive the runs are on */
#define VCD_OFF_NRUNS    0x0006   /* word */
#define VCD_OFF_PERCD    0x0008   /* word: disk sectors per CD sector */
#define VCD_OFF_NCD      0x000A   /* dword: CD sectors in the medium */
#define VCD_OFF_NOVER    0x000E   /* word: how many overrides */
#define VCD_OFF_NREAL    0x0028   /* dword: sectors the runs hold */
#define VCD_OFF_NLIMIT   0x002C   /* dword: the last it answers for */
#define VCD_OFF_DAP      0x0010   /* 16 bytes: the packet we re-issue with */
#define VCD_OFF_END      0x0020   /* word: the handler's two scratch slots */
#define VCD_OFF_GAP      0x0024   /* dword */
/* An override is lba(4) count(4) segment(2) flags(2) phys(4). */
#define VCD_OFF_OVER     0x0030   /* 8 overrides of 16 bytes */
#define VCD_OV_LEN       16
#define VCD_OV_EXT       1        /* flags bit: bytes are at phys */
#define VCD_MAX_OVER_ASM 8

/* The six descriptors INT 15h AH=87h reads to move bytes the far side of a
 * megabyte. Only the second and third are ours; the BIOS fills the rest. */
#define VCD_OFF_MOVEGDT  0x00B0   /* 48 bytes */

/* The INT 15h half (DESIGN Section 11.3.8): the vector it chains to, how many
 * entries of the memory map it answers E820 with (0: it answers none and the
 * BIOS answers them all), and the ceilings on E801's two counts: 64 KB
 * blocks above 16 MB, and KB between 1 and 16 MB. */
#define VCD_OFF_OLDINT15 0x00E0   /* far pointer to the previous handler */
#define VCD_OFF_NE820    0x00E4   /* word */
#define VCD_OFF_E801CAP  0x00E6   /* word */
#define VCD_OFF_E801LOW  0x00E8   /* word: the 1-16 MB half's ceiling, KB */
#define VCD_OFF_RUNS     0x0100   /* 64 runs of 16 bytes */
#define VCD_RUN_LEN      16
#define VCD_MAX_RUNS_ASM 64
#define VCD_OFF_SPEC     0x0500   /* 20 bytes: the El Torito status packet */

/* The launch stub and what it needs. It is inside the resident for the one
 * reason that matters: it copies the medium's boot image to where El Torito
 * says it goes, which is 0x7C00, and everything from there up is stage 2 -
 * the gate, the thunk, the code doing the copying. A stub anywhere else
 * would be overwriting itself. Here it is in memory the BIOS has been told
 * does not exist, so nothing it copies can reach it. */
#define VCD_OFF_LSRC     0x0520   /* word: segment the image is staged at */
#define VCD_OFF_LDST     0x0522   /* word: segment it belongs at */
#define VCD_OFF_LLEN     0x0524   /* word: bytes */
#define VCD_OFF_LDRIVE   0x0526   /* byte: what to hand over in DL */
#define VCD_OFF_LAUNCH   0x0530
#define VCD_OFF_CODE     0x0570   /* the INT 13h entry: the header, then */
                                  /* the handler at + SMH_BYTES */
#define VCD_OFF_OLDINT13 (VCD_OFF_CODE + SMH_OFF_NEXT) /* far pointer to */
                                  /* the previous handler */

/* The INT 15h entry, and the memory map it answers with: the BIOS's own,
 * with the resident and anything it serves from extended memory carved out
 * of it (e820_carve). Twenty-byte entries, the form without ACPI 3.0's
 * attributes word, which every caller accepts. 64 of them is what fits the
 * block, and a real map is a few dozen at most. */
#define VCD_OFF_CODE15   0x0A00
#define VCD_OFF_E820     0x0B00
#define VCD_E820_LEN     20
#define VCD_E820_MAX     64

/* The resident is one 4 KB block, its memory map filling the last of it;
 * the twelve kilobytes after it are the
 * overlay pool, the bytes an override serves. Sixteen kilobytes is what the
 * BIOS data area is told the machine no longer has. */
#define VCD_OVER_OFF     0x1000
#define VCD_OVER_MAX_CD  6        /* 12 KB, in CD sectors */
#define VCD_BYTES        0x4000   /* 16 KB, which is what the BDA loses */
#define VCD_CD_BYTES     2048     /* a CD sector, and what AH=48h reports */

#define VCD_STAGE_ADDR   0x00020000

/* The drive swap (falcon-bios/src/mbr/swapres.S, chain.c; DESIGN Section
 * 12.4): the station's one resident, an INT 13h hook that exchanges one
 * disk's number with 0x80 on every call, so a system chained off a second
 * disk finds itself at 0x80 as it would had the BIOS booted it. The whole
 * block is the old vector, the drive and the handler, under a hundred
 * bytes; SWAP_BYTES is what the BDA loses, since conventional memory is
 * taken in whole kilobytes. The offsets, as swapres.S lays them out with
 * .org and the build checks: */
#define SWAP_OFF_DRIVE    0x0004  /* byte: the disk exchanged with 0x80 */
#define SWAP_OFF_CODE     0x0010  /* the INT 13h entry: the header, then */
                                  /* the handler at + SMH_BYTES */
#define SWAP_OFF_OLDINT13 (SWAP_OFF_CODE + SMH_OFF_NEXT) /* far pointer to */
                                  /* the previous handler */
#define SWAP_BYTES        0x0400  /* 1 KB, what the BDA loses */

/* The floppy-emulation resident (falcon-bios/src/vfd/vfdres.S, vfd.c), for a
 * medium whose El Torito BIOS entry is a floppy image rather than a
 * no-emulation CD. It is a memory disk: the whole image (2.88 MB at most)
 * sits in extended memory and INT 15h AH=87h copies from it, so the block
 * itself is small. It never coexists with the CD resident - a medium is one
 * or the other - so it takes the same top-of-conventional-memory block and
 * the same VCD_BYTES the BDA gives up.
 *
 * It hooks INT 13h only. The image is served out of extended memory, which
 * the map handed to the payload does not hide: the media this route serves
 * (memtest86+, Haiku) read the floppy once as they load and never again, so
 * that RAM being usable afterwards is harmless, and not carving the map
 * keeps the resident to one interrupt. The offsets, as vfdres.S lays them
 * out with .org and the build checks: */
#define VFD_OFF_DRIVE    0x0008   /* byte: the drive we answer for (0x00) */
#define VFD_OFF_HEADS    0x0009   /* byte */
#define VFD_OFF_SECTORS  0x000A   /* byte: sectors per track */
#define VFD_OFF_BTYPE    0x000B   /* byte: AH=08h/AH=15h drive type */
#define VFD_OFF_CYLS     0x000C   /* word */
#define VFD_OFF_TOTAL    0x0010   /* dword: total 512-byte sectors */
#define VFD_OFF_PHYS     0x0014   /* dword: the image's physical address */
#define VFD_OFF_STATUS   0x001C   /* byte: the last operation's status */
#define VFD_OFF_SRC      0x0020   /* dword: scratch, an AH=87h source */
#define VFD_OFF_DST      0x0024   /* dword: scratch, an AH=87h destination */
#define VFD_OFF_CNT      0x0028   /* dword: scratch, a read's sector count */
#define VFD_OFF_MOVEGDT  0x0040   /* 48 bytes: the AH=87h descriptor table */
#define VFD_OFF_DBT      0x0070   /* 11 bytes: the disk base table AH=08h returns */
#define VFD_OFF_LDRIVE   0x007C   /* byte: the drive handed over in DL */
#define VFD_OFF_LAUNCH   0x0080   /* the launch stub */
#define VFD_OFF_CODE     0x00C0   /* the INT 13h entry: the header, then */
                                  /* the handler at + SMH_BYTES */
#define VFD_OFF_OLDINT13 (VFD_OFF_CODE + SMH_OFF_NEXT) /* far pointer to */
                                  /* the previous handler */

/* Where sector 0 of the image is staged for the launch stub to copy to
 * 0x7C00: the same conventional address the CD's boot image uses, since the
 * two routes never run together. */
#define VFD_STAGE_ADDR   VCD_STAGE_ADDR

/* ---- falcon jump's modules (docs/DESIGN.md Section 13.4) ----
 *
 * A driver is a flat image linked at a fixed slot, read off the floppy into
 * that slot, and left there for as long as it serves a drive. The slots are
 * one small region just under KERNEL_ADDR, so any machine with 16 MB has
 * them, and above everything a falcon-bios stage 3 occupies (its stack ends
 * at STAGE3_STACK), so a stick's loader started through a module never lands
 * on one. The region is carved out of the memory map the next loader is
 * handed. Each slot holds the module's image, its .bss and its stack, which
 * is the top JMOD_STACK_BYTES of the slot. */
#define JMOD_REGION      0x00F00000
#define JMOD_SLOT_BYTES  0x00020000   /* 128 KB */
#define JMOD_SLOTS       8
#define JMOD_REGION_END  (JMOD_REGION + JMOD_SLOTS * JMOD_SLOT_BYTES)
#define JMOD_STACK_BYTES 0x2000
#define JMOD_SLOT_ADDR(n) (JMOD_REGION + (n) * JMOD_SLOT_BYTES)
#define JMOD_SLOT_ATA    0
#define JMOD_SLOT_UHCI   1
#define JMOD_SLOT_OHCI   2
#define JMOD_SLOT_EHCI   3
#define JMOD_SLOT_USB    4
#define JMOD_SLOT_PCC    5

/* A module is stored on the floppy as a gzip member (DESIGN Section 13.3),
 * read here whole and inflated into its slot. GZ_STAGE_ADDR again: stage 2
 * is done with it by the time stage 3 runs, and it is inside the memory
 * stage 3 has to itself, below STAGE3_STACK and above the image, which
 * load3.c keeps below GZ_STAGE_ADDR. */
#define JMOD_GZ_STAGE    GZ_STAGE_ADDR

/* The reverse thunk (falcon-bios/src/jump/rthunk.S, jres.c): the real-mode
 * INT 13h and INT 16h entries a module's resident half is reached through.
 * One block in
 * conventional memory, taken from the BDA's count as the virtual CD's is,
 * data at fixed offsets and then code, position independent: it is copied
 * wherever the count leaves room. The far pointers are part-filled by the
 * assembly (their offsets) and part by the installer (the block's segment
 * and linear address). The build checks the offsets against the .org's. */
#define JRES_OFF_DRIVE    0x04   /* byte: the drive the module answers */
#define JRES_OFF_A20      0x05   /* byte: port 0x92 as the caller had it */
#define JRES_OFF_GDTR     0x08   /* 6 bytes: this block's GDT */
#define JRES_OFF_SAVEGDT  0x10   /* 6 bytes: the caller's GDTR */
#define JRES_OFF_SAVESP   0x18   /* word */
#define JRES_OFF_SAVESS   0x1A   /* word */
#define JRES_OFF_ENTRY    0x1C   /* dword: the module's int13, linear */
#define JRES_OFF_STACK    0x20   /* dword: the module's stack top, linear */
#define JRES_OFF_PM32PTR  0x28   /* 6 bytes: offset32, selector 0x08 */
#define JRES_OFF_RMPTR    0x30   /* 4 bytes: offset16, segment */
#define JRES_OFF_PM16PTR  0x34   /* 6 bytes: offset32, selector 0x18 */
#define JRES_OFF_NE820    0x3A   /* word: carved entries; 0 hooks no INT 15h */
#define JRES_OFF_E801LOW  0x3C   /* word: E801's 1-16 MB ceiling, and 88h's */
#define JRES_OFF_E801CAP  0x3E   /* word: E801's above-16 MB ceiling */
#define JRES_OFF_GDT      0x40   /* 5 descriptors */
#define JRES_OFF_OLDINT15 0x68   /* far pointer to the previous INT 15h */
#define JRES_OFF_REGS     0x70   /* jmod_regs, 32 bytes */
#define JRES_OFF_CODE     0x90   /* the INT 13h entry: the header, then */
                                 /* the handler at + SMH_BYTES */
#define JRES_OFF_OLDINT13 (JRES_OFF_CODE + SMH_OFF_NEXT) /* far pointer to */
                                 /* the previous handler */
/* The INT 16h entry (the USB route's, when a module found a keyboard):
 * the keyboards polled, then the BIOS's handler. */
#define JRES_OFF_CODE16   0x240
/* The INT 15h half (the USB route's; the CD route's virtual CD has its own):
 * the memory map with the resident's memory carved out of it, as
 * vcdres.S's, in the same twenty-byte entries. */
#define JRES_OFF_CODE15   0x300
#define JRES_OFF_E820     0x400
#define JRES_E820_MAX     64
/* The INT 18h and INT 19h entry: a medium that gives up, or asks for the
 * bootstrap again, gets a cold reset instead of the BIOS's next boot device
 * started on top of this block, its hooks and a powered PC Card slot. After
 * the carved map's 64 entries. */
#define JRES_OFF_RESET    0x900
/* The release record: what jres_install changed outside the block, so
 * whoever runs after the route (jump's own failure path, or station-legacy
 * chaining a system on another disk) can put the machine back as the BIOS
 * left it. JRES_MAGIC is how a program that did not install the block
 * recognises it behind the INT 13h vector. After the reset code. */
#define JRES_OFF_MAGIC    0xB00  /* 4 bytes, JRES_MAGIC */
#define JRES_OFF_OLDINT18 0xB04  /* far pointer */
#define JRES_OFF_OLDINT19 0xB08  /* far pointer */
#define JRES_OFF_OLDKB    0xB0C  /* word: the BDA's memory count before */
#define JRES_OFF_OLDNDISK 0xB0E  /* byte: the BDA's disk count before */
#define JRES_OFF_OLDINT16 0xB10  /* far pointer */
#define JRES_OFF_NKBD     0xB14  /* byte: keyboard entries; 0 hooks no INT 16h */
#define JRES_OFF_CALL     0xB15  /* byte: 0 the INT 13h entry, 1 the keyboards */
#define JRES_OFF_KBD      0xB18  /* dwords: the modules' int16, linear */
#define JRES_KBD_MAX      3
#define JRES_MAGIC        0x53524A46u  /* "FJRS", little-endian */
#define JRES_BYTES        0xC00  /* 3 KB, what the BDA loses */

/* Stage 3's stack. Above the image and below anything a payload is loaded
 * into (KERNEL_ADDR is 16 MB). Stage 3's image and .bss end below
 * GZ_STAGE_ADDR, and the rest up to here is its to use. */
#define STAGE3_STACK    0x00800000

/* A shelf's journal table (falcon-core/src/fs/jbd2.h): the 7 MB between
 * stage 3's stack and falcon jump's module slots. Every read of a replayed
 * shelf goes through the table, including the kernel's and the initrd's, so
 * it must be clear of where those land: the kernel is read at KERNEL_ADDR or
 * staged above its own footprint there, and the initrd above that, so
 * nothing this route reads lands in it. A kernel that runs at 1 MB is moved
 * down over it only at the handoff, after the last read. Not the heap: that
 * starts at 64 MB, which a machine the BIOS station runs on may not have,
 * and is placed as high as the initrd is. 7 MB is a table for a journal of
 * about 300,000 blocks; a shelf's is a few thousand. */
#define JNL_REGION      STAGE3_STACK
#define JNL_REGION_END  JMOD_REGION

/* COM1 and 115200 8N1. The divisor is 115200 / 115200 = 1.
 *
 * Serial rather than INT 10h for anything a test reads: the rig captures it
 * (falcon-efi/tools/run-qemu.sh --bios --headless), and a machine with no
 * usable video is exactly the machine this route exists for. */
#define SERIAL_PORT     0x3F8
#define SERIAL_DIVISOR  1
/* How many reads of the line status register to wait for the transmitter
 * before writing anyway: about a microsecond each, against 87 for one
 * character at 115200. Only a UART that is present and never empties runs it
 * out, and every stage bounds its wait by it (stage 1 by the 16 bits it has),
 * because COM1 is a log and must not be able to stop a boot. */
#define SERIAL_SPINS    65536

#endif /* FALCON_BIOSDEFS_H */
