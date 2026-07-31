#include "WolfSshUefi.h"

#include <Protocol/SimpleTextIn.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <wolfssl/wolfcrypt/sha256.h>

STATIC
BOOLEAN
ReadConfirmation (
  IN WOLFSSH_UEFI_TCP4  *Socket
  )
{
  EFI_INPUT_KEY Key;
  EFI_STATUS    Status;

  for (;;) {
    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    if (Status == EFI_NOT_READY) {
      WolfSshTcp4Poll (Socket);
      gBS->Stall (5000);
      continue;
    }
    if (EFI_ERROR (Status)) {
      Print (L"\r\n");
      return FALSE;
    }
    if ((Key.UnicodeChar == L'y') || (Key.UnicodeChar == L'Y')) {
      Print (L"y\r\n");
      return TRUE;
    }
    if ((Key.UnicodeChar == CHAR_CARRIAGE_RETURN) ||
        (Key.UnicodeChar == L'n') || (Key.UnicodeChar == L'N') ||
        (Key.UnicodeChar == 0x1b)) {
      Print (L"n\r\n");
      return FALSE;
    }
  }
}

STATIC
EFI_STATUS
ReadPassword (
  IN OUT WOLFSSH_UEFI_AUTH  *Auth
  )
{
  EFI_INPUT_KEY Key;
  EFI_STATUS    Status;
  UINTN         Length;

  Length = 0;
  ZeroMem (Auth->Options->Password, sizeof (Auth->Options->Password));
  Print (L"Password for %a: ", Auth->Options->Username);
  for (;;) {
    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    if (Status == EFI_NOT_READY) {
      WolfSshTcp4Poll (Auth->Socket);
      gBS->Stall (5000);
      continue;
    }
    if (EFI_ERROR (Status)) {
      Print (L"\r\n");
      return Status;
    }
    if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      Auth->Options->Password[Length] = '\0';
      Auth->Options->HavePassword = TRUE;
      Print (L"\r\n");
      return EFI_SUCCESS;
    }
    if ((Key.UnicodeChar == CHAR_BACKSPACE) && (Length != 0)) {
      Length--;
      Auth->Options->Password[Length] = '\0';
      continue;
    }
    if ((Key.UnicodeChar >= 0x20) && (Key.UnicodeChar <= 0x7e) &&
        (Length + 1 < sizeof (Auth->Options->Password))) {
      Auth->Options->Password[Length++] = (CHAR8)Key.UnicodeChar;
    }
  }
}

int
WolfSshHostKeyCallback (
  const byte  *PublicKey,
  word32      PublicKeySize,
  void        *Context
  )
{
  WOLFSSH_UEFI_AUTH *Auth;
  wc_Sha256         Sha;
  UINT8             Digest[WC_SHA256_DIGEST_SIZE];
  CHAR8             Hex[WOLFSSH_UEFI_FINGERPRINT_SIZE];
  UINTN             Index;
  INT32             Result;

  Auth = (WOLFSSH_UEFI_AUTH *)Context;
  if ((Auth == NULL) || (Auth->Options == NULL) ||
      (PublicKey == NULL) || (PublicKeySize == 0)) {
    return -1;
  }
  if (Auth->HostKeyChecked) {
    return Auth->HostKeyAccepted ? 0 : -1;
  }

  Result = wc_InitSha256 (&Sha);
  if (Result == 0) {
    Result = wc_Sha256Update (&Sha, PublicKey, PublicKeySize);
  }
  if (Result == 0) {
    Result = wc_Sha256Final (&Sha, Digest);
  }
  wc_Sha256Free (&Sha);
  if (Result != 0) {
    return -1;
  }

  for (Index = 0; Index < sizeof (Digest); Index++) {
    Hex[Index * 2] = "0123456789abcdef"[Digest[Index] >> 4];
    Hex[Index * 2 + 1] = "0123456789abcdef"[Digest[Index] & 0x0f];
  }
  Hex[64] = '\0';

  Auth->HostKeyChecked = TRUE;
  Print (L"Server host-key SHA-256: %a\r\n", Hex);
  if (Auth->Options->HaveFingerprint) {
    Auth->HostKeyAccepted =
      (BOOLEAN)(AsciiStrCmp (Hex, Auth->Options->Fingerprint) == 0);
    if (!Auth->HostKeyAccepted) {
      Print (L"Host-key fingerprint mismatch; connection rejected.\r\n");
    }
  } else if (Auth->Options->AcceptAnyHostKey) {
    Print (L"Warning: accepting an unpinned host key for this connection.\r\n");
    Auth->HostKeyAccepted = TRUE;
  } else {
    Print (L"Accept this host key for this boot? [y/N] ");
    Auth->HostKeyAccepted = ReadConfirmation (Auth->Socket);
  }

  return Auth->HostKeyAccepted ? 0 : -1;
}

int
WolfSshUserAuthCallback (
  byte             AuthType,
  WS_UserAuthData  *AuthData,
  void             *Context
  )
{
  WOLFSSH_UEFI_AUTH *Auth;
  UINTN             PasswordLength;

  Auth = (WOLFSSH_UEFI_AUTH *)Context;
  if ((Auth == NULL) || (Auth->Options == NULL) || (AuthData == NULL)) {
    return WOLFSSH_USERAUTH_FAILURE;
  }
  if (AuthType != WOLFSSH_USERAUTH_PASSWORD) {
    return WOLFSSH_USERAUTH_FAILURE;
  }

  if (!Auth->Options->HavePassword && !Auth->PasswordPrompted) {
    Auth->PasswordPrompted = TRUE;
    if (EFI_ERROR (ReadPassword (Auth))) {
      return WOLFSSH_USERAUTH_FAILURE;
    }
  }
  if (!Auth->Options->HavePassword) {
    return WOLFSSH_USERAUTH_FAILURE;
  }

  PasswordLength = AsciiStrLen (Auth->Options->Password);
  AuthData->sf.password.password = (CONST byte *)Auth->Options->Password;
  AuthData->sf.password.passwordSz = (word32)PasswordLength;
  return WOLFSSH_USERAUTH_SUCCESS;
}
