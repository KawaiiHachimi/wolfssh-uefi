#ifndef UEFI_COMPAT_LIB_H
#define UEFI_COMPAT_LIB_H

#include <Uefi.h>

VOID *EFIAPI UefiCompatAllocate (IN UINTN Size);
VOID EFIAPI UefiCompatFree (IN VOID *Buffer);
VOID *EFIAPI UefiCompatReallocate (IN VOID *Buffer, IN UINTN NewSize);
int UefiRandomGenerateBlock (unsigned char *Output, unsigned int Size);

#endif
