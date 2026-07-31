#include <Uefi.h>

#include <Protocol/Rng.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define WOLF_ALLOC_SIGNATURE SIGNATURE_64 ('W', 'S', 'S', 'H', 'A', 'L', 'L', 'C')

typedef struct {
  UINT64 Signature;
  UINTN  Size;
} WOLF_ALLOC_HEADER;

STATIC EFI_RNG_PROTOCOL *mRng;

void
wolfSSH_UEFI_GetTerminalInfo (
  unsigned int  *Width,
  unsigned int  *Height,
  unsigned int  *PixelWidth,
  unsigned int  *PixelHeight,
  const char    **TerminalType
  )
{
  UINTN      Columns;
  UINTN      Rows;
  EFI_STATUS Status;

  if ((Width == NULL) || (Height == NULL) || (PixelWidth == NULL) ||
      (PixelHeight == NULL) || (TerminalType == NULL)) {
    return;
  }

  *Width        = 80;
  *Height       = 25;
  *PixelWidth   = 0;
  *PixelHeight  = 0;
  *TerminalType = "xterm-256color";

  if ((gST == NULL) || (gST->ConOut == NULL) ||
      (gST->ConOut->Mode == NULL)) {
    return;
  }

  Status = gST->ConOut->QueryMode (
                         gST->ConOut,
                         gST->ConOut->Mode->Mode,
                         &Columns,
                         &Rows
                         );
  if (!EFI_ERROR (Status) && (Columns != 0) && (Rows != 0) &&
      (Columns <= MAX_UINT32) && (Rows <= MAX_UINT32)) {
    *Width  = (unsigned int)Columns;
    *Height = (unsigned int)Rows;
  }
}

VOID *
EFIAPI
UefiCompatAllocate (
  IN UINTN Size
  )
{
  WOLF_ALLOC_HEADER *Header;

  if (Size > MAX_UINTN - sizeof (*Header)) {
    return NULL;
  }

  Header = AllocatePool (sizeof (*Header) + Size);
  if (Header == NULL) {
    return NULL;
  }

  Header->Signature = WOLF_ALLOC_SIGNATURE;
  Header->Size      = Size;
  return Header + 1;
}

VOID
EFIAPI
UefiCompatFree (
  IN VOID *Buffer
  )
{
  WOLF_ALLOC_HEADER *Header;

  if (Buffer == NULL) {
    return;
  }

  Header = (WOLF_ALLOC_HEADER *)Buffer - 1;
  if (Header->Signature != WOLF_ALLOC_SIGNATURE) {
    return;
  }

  Header->Signature = 0;
  FreePool (Header);
}

VOID *
EFIAPI
UefiCompatReallocate (
  IN VOID  *Buffer,
  IN UINTN NewSize
  )
{
  WOLF_ALLOC_HEADER *OldHeader;
  VOID              *NewBuffer;
  UINTN             CopySize;

  if (Buffer == NULL) {
    return UefiCompatAllocate (NewSize);
  }
  if (NewSize == 0) {
    UefiCompatFree (Buffer);
    return NULL;
  }

  OldHeader = (WOLF_ALLOC_HEADER *)Buffer - 1;
  if (OldHeader->Signature != WOLF_ALLOC_SIGNATURE) {
    return NULL;
  }

  NewBuffer = UefiCompatAllocate (NewSize);
  if (NewBuffer == NULL) {
    return NULL;
  }

  CopySize = MIN (OldHeader->Size, NewSize);
  CopyMem (NewBuffer, Buffer, CopySize);
  UefiCompatFree (Buffer);
  return NewBuffer;
}

void *
UefiWolfAllocate (
  unsigned long Size
  )
{
  return UefiCompatAllocate ((UINTN)Size);
}

void
UefiWolfFree (
  void *Buffer
  )
{
  UefiCompatFree (Buffer);
}

void *
UefiWolfReallocate (
  void          *Buffer,
  unsigned long Size
  )
{
  return UefiCompatReallocate (Buffer, (UINTN)Size);
}

int
UefiRandomGenerateBlock (
  unsigned char *Output,
  unsigned int  Size
  )
{
  EFI_STATUS Status;

  if ((Output == NULL) && (Size != 0)) {
    return -1;
  }
  if (mRng == NULL) {
    Status = gBS->LocateProtocol (&gEfiRngProtocolGuid, NULL, (VOID **)&mRng);
    if (EFI_ERROR (Status)) {
      return -1;
    }
  }

  Status = mRng->GetRNG (mRng, NULL, (UINTN)Size, Output);
  return EFI_ERROR (Status) ? -1 : 0;
}

void *
memcpy (
  void        *Destination,
  const void  *Source,
  UINTN       Length
  )
{
  return CopyMem (Destination, Source, Length);
}

void *
memmove (
  void        *Destination,
  const void  *Source,
  UINTN       Length
  )
{
  return CopyMem (Destination, Source, Length);
}

void *
memset (
  void   *Buffer,
  int    Value,
  UINTN  Length
  )
{
  SetMem (Buffer, Length, (UINT8)Value);
  return Buffer;
}

int
memcmp (
  const void  *First,
  const void  *Second,
  UINTN       Length
  )
{
  CONST UINT8 *Left;
  CONST UINT8 *Right;
  UINTN       Index;

  Left = (CONST UINT8 *)First;
  Right = (CONST UINT8 *)Second;
  for (Index = 0; Index < Length; Index++) {
    if (Left[Index] != Right[Index]) {
      return (int)Left[Index] - (int)Right[Index];
    }
  }
  return 0;
}

UINTN
strlen (
  const char  *String
  )
{
  CONST char *End;

  End = String;
  while (*End != '\0') {
    End++;
  }
  return (UINTN)(End - String);
}

int
strcmp (
  const char  *First,
  const char  *Second
  )
{
  while ((*First != '\0') && (*First == *Second)) {
    First++;
    Second++;
  }
  return (int)(UINT8)*First - (int)(UINT8)*Second;
}

int
strncmp (
  const char  *First,
  const char  *Second,
  UINTN       Length
  )
{
  while ((Length != 0) && (*First != '\0') && (*First == *Second)) {
    First++;
    Second++;
    Length--;
  }
  if (Length == 0) {
    return 0;
  }
  return (int)(UINT8)*First - (int)(UINT8)*Second;
}

char *
strncpy (
  char        *Destination,
  const char  *Source,
  UINTN       Length
  )
{
  char  *Result;

  Result = Destination;
  while ((Length != 0) && (*Source != '\0')) {
    *Destination++ = *Source++;
    Length--;
  }
  while (Length-- != 0) {
    *Destination++ = '\0';
  }
  return Result;
}

char *
strncat (
  char        *Destination,
  const char  *Source,
  UINTN       Length
  )
{
  char *Result;

  Result = Destination;
  Destination += strlen (Destination);
  while ((Length-- != 0) && (*Source != '\0')) {
    *Destination++ = *Source++;
  }
  *Destination = '\0';
  return Result;
}
