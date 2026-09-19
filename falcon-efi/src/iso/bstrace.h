/* TRACE BUILD ONLY (branch trace/esxi). See bstrace.c. */
#ifndef FALCON_BSTRACE_H
#define FALCON_BSTRACE_H
#include <efi/uefi.h>
void bstrace_on(EFI_SYSTEM_TABLE *st);
void bstrace_off(EFI_SYSTEM_TABLE *st);
void bstrace_blockio(EFI_SYSTEM_TABLE *st);
void bstrace_image(EFI_SYSTEM_TABLE *st, EFI_HANDLE img);
#endif
