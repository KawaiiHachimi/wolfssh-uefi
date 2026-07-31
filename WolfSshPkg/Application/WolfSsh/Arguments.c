#include "WolfSshUefi.h"

#include <Protocol/ShellParameters.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/NetLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

STATIC
EFI_STATUS
CopyUnicodeAsAscii (
  IN CONST CHAR16  *Source,
  OUT CHAR8        *Destination,
  IN UINTN         Capacity
  )
{
  UINTN Index;

  if ((Source == NULL) || (Destination == NULL) || (Capacity == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  for (Index = 0; Source[Index] != L'\0'; Index++) {
    if ((Index + 1 >= Capacity) || (Source[Index] > 0x7f)) {
      Destination[0] = '\0';
      return EFI_INVALID_PARAMETER;
    }
    Destination[Index] = (CHAR8)Source[Index];
  }
  Destination[Index] = '\0';
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ParsePort (
  IN CONST CHAR16  *Text,
  OUT UINT16       *Port
  )
{
  UINTN Value;
  UINTN Index;

  if ((Text == NULL) || (*Text == L'\0') || (Port == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Value = 0;
  for (Index = 0; Text[Index] != L'\0'; Index++) {
    if ((Text[Index] < L'0') || (Text[Index] > L'9')) {
      return EFI_INVALID_PARAMETER;
    }
    Value = Value * 10 + (UINTN)(Text[Index] - L'0');
    if (Value > MAX_UINT16) {
      return EFI_INVALID_PARAMETER;
    }
  }
  if (Value == 0) {
    return EFI_INVALID_PARAMETER;
  }
  *Port = (UINT16)Value;
  return EFI_SUCCESS;
}

STATIC
INTN
HexValue (
  IN CHAR16 Character
  )
{
  if ((Character >= L'0') && (Character <= L'9')) {
    return Character - L'0';
  }
  if ((Character >= L'a') && (Character <= L'f')) {
    return Character - L'a' + 10;
  }
  if ((Character >= L'A') && (Character <= L'F')) {
    return Character - L'A' + 10;
  }
  return -1;
}

STATIC
EFI_STATUS
ParseFingerprint (
  IN CONST CHAR16  *Text,
  OUT CHAR8        *Fingerprint
  )
{
  UINTN InputIndex;
  UINTN OutputIndex;
  INTN  Value;

  if ((Text == NULL) || (Fingerprint == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  OutputIndex = 0;
  for (InputIndex = 0; Text[InputIndex] != L'\0'; InputIndex++) {
    if ((Text[InputIndex] == L':') || (Text[InputIndex] == L'-')) {
      continue;
    }
    Value = HexValue (Text[InputIndex]);
    if ((Value < 0) || (OutputIndex >= 64)) {
      return EFI_INVALID_PARAMETER;
    }
    Fingerprint[OutputIndex++] = "0123456789abcdef"[Value];
  }
  if (OutputIndex != 64) {
    return EFI_INVALID_PARAMETER;
  }
  Fingerprint[OutputIndex] = '\0';
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ParseTarget (
  IN CONST CHAR16          *Text,
  IN OUT WOLFSSH_UEFI_OPTIONS  *Options,
  IN BOOLEAN               HaveSeparateUsername
  )
{
  CHAR8 Target[128];
  CHAR8 *At;
  CHAR8 *Host;
  UINTN Length;

  if (EFI_ERROR (CopyUnicodeAsAscii (Text, Target, sizeof (Target)))) {
    return EFI_INVALID_PARAMETER;
  }

  At = AsciiStrStr (Target, "@");
  Host = Target;
  if (At != NULL) {
    *At = '\0';
    Host = At + 1;
    Length = AsciiStrLen (Target);
    if ((Length == 0) || (Length >= sizeof (Options->Username))) {
      return EFI_INVALID_PARAMETER;
    }
    CopyMem (Options->Username, Target, Length + 1);
  } else if (!HaveSeparateUsername) {
    return EFI_INVALID_PARAMETER;
  }

  if ((*Host == '\0') || EFI_ERROR (NetLibAsciiStrToIp4 (Host, &Options->Address))) {
    return EFI_INVALID_PARAMETER;
  }
  return EFI_SUCCESS;
}

VOID
WolfSshPrintUsage (
  VOID
  )
{
  Print (L"wolfssh-uefi 0.1 - interactive SSH client for UEFI Shell\r\n");
  Print (L"Usage: wolfssh.efi [-p port] [-P password] [-f sha256hex | -y] user@IPv4\r\n");
  Print (L"       wolfssh.efi -l user [-p port] IPv4\r\n");
  Print (L"       wolfssh.efi --self-test\r\n\r\n");
  Print (L"  -p port       SSH port (default 22)\r\n");
  Print (L"  -P password   Password; omit to enter it without echo\r\n");
  Print (L"  -f digest     Required SHA-256 host-key digest (64 hex digits)\r\n");
  Print (L"  -y            Accept the presented host key for this connection\r\n");
  Print (L"  -l user       Username when target is specified without user@\r\n");
  Print (L"  Ctrl+]        Leave the remote terminal\r\n");
}

EFI_STATUS
WolfSshParseArguments (
  IN EFI_HANDLE             ImageHandle,
  OUT WOLFSSH_UEFI_OPTIONS  *Options
  )
{
  EFI_SHELL_PARAMETERS_PROTOCOL *Parameters;
  EFI_STATUS                    Status;
  CONST CHAR16                  *Target;
  UINTN                         Index;
  BOOLEAN                       HaveUsername;

  if (Options == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  ZeroMem (Options, sizeof (*Options));
  Options->Port = 22;

  Status = gBS->HandleProtocol (
                  ImageHandle,
                  &gEfiShellParametersProtocolGuid,
                  (VOID **)&Parameters
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Target = NULL;
  HaveUsername = FALSE;
  for (Index = 1; Index < Parameters->Argc; Index++) {
    if ((StrCmp (Parameters->Argv[Index], L"-h") == 0) ||
        (StrCmp (Parameters->Argv[Index], L"--help") == 0) ||
        (StrCmp (Parameters->Argv[Index], L"-?") == 0)) {
      Options->Help = TRUE;
      continue;
    }
    if (StrCmp (Parameters->Argv[Index], L"--self-test") == 0) {
      Options->SelfTest = TRUE;
      continue;
    }
    if (StrCmp (Parameters->Argv[Index], L"-y") == 0) {
      Options->AcceptAnyHostKey = TRUE;
      continue;
    }
    if ((StrCmp (Parameters->Argv[Index], L"-p") == 0) ||
        (StrCmp (Parameters->Argv[Index], L"-P") == 0) ||
        (StrCmp (Parameters->Argv[Index], L"-f") == 0) ||
        (StrCmp (Parameters->Argv[Index], L"-l") == 0)) {
      CONST CHAR16 *Value;
      CONST CHAR16 *Flag;

      Flag = Parameters->Argv[Index];
      if (++Index >= Parameters->Argc) {
        return EFI_INVALID_PARAMETER;
      }
      Value = Parameters->Argv[Index];
      if (StrCmp (Flag, L"-p") == 0) {
        if (EFI_ERROR (ParsePort (Value, &Options->Port))) {
          return EFI_INVALID_PARAMETER;
        }
      } else if (StrCmp (Flag, L"-P") == 0) {
        if (EFI_ERROR (CopyUnicodeAsAscii (
                         Value,
                         Options->Password,
                         sizeof (Options->Password)
                         ))) {
          return EFI_INVALID_PARAMETER;
        }
        Options->HavePassword = TRUE;
      } else if (StrCmp (Flag, L"-f") == 0) {
        if (EFI_ERROR (ParseFingerprint (Value, Options->Fingerprint))) {
          return EFI_INVALID_PARAMETER;
        }
        Options->HaveFingerprint = TRUE;
      } else {
        if (EFI_ERROR (CopyUnicodeAsAscii (
                         Value,
                         Options->Username,
                         sizeof (Options->Username)
                         ))) {
          return EFI_INVALID_PARAMETER;
        }
        HaveUsername = TRUE;
      }
      continue;
    }
    if ((Parameters->Argv[Index][0] == L'-') || (Target != NULL)) {
      return EFI_INVALID_PARAMETER;
    }
    Target = Parameters->Argv[Index];
  }

  if (Options->Help || Options->SelfTest) {
    return EFI_SUCCESS;
  }
  if (Target == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Status = ParseTarget (Target, Options, HaveUsername);
  if (EFI_ERROR (Status) || (Options->Username[0] == '\0')) {
    return EFI_INVALID_PARAMETER;
  }
  if (Options->AcceptAnyHostKey && Options->HaveFingerprint) {
    return EFI_INVALID_PARAMETER;
  }
  return EFI_SUCCESS;
}
