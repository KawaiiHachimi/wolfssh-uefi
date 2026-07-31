#include "WolfSshUefi.h"

#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiCompatLib.h>
#include <Library/UefiLib.h>
#include <Library/WolfCryptLib.h>
#include <Library/WolfSshLib.h>

#include <wolfssh/error.h>

#define SSH_CONNECT_RETRY_LIMIT 12000
#define SSH_SEND_RETRY_LIMIT     2000

STATIC
BOOLEAN
IsRetryableError (
  IN INT32 Error
  )
{
  return (BOOLEAN)(
           (Error == WS_WANT_READ) ||
           (Error == WS_WANT_WRITE) ||
           (Error == WS_WINDOW_FULL) ||
           (Error == WS_REKEYING)
           );
}

STATIC
EFI_STATUS
SendAll (
  IN WOLFSSH                 *Ssh,
  IN OUT WOLFSSH_UEFI_TCP4   *Socket,
  IN CONST UINT8             *Buffer,
  IN UINTN                   Length
  )
{
  UINTN Offset;
  UINTN Retry;
  INT32 Result;
  INT32 Error;

  Offset = 0;
  Retry = 0;
  while (Offset < Length) {
    Result = wolfSSH_stream_send (
               Ssh,
               (byte *)(Buffer + Offset),
               (word32)(Length - Offset)
               );
    if (Result > 0) {
      Offset += (UINTN)Result;
      Retry = 0;
      continue;
    }
    Error = wolfSSH_get_error (Ssh);
    if (!IsRetryableError (Error) || (++Retry > SSH_SEND_RETRY_LIMIT)) {
      return EFI_DEVICE_ERROR;
    }
    if (Error == WS_REKEYING) {
      wolfSSH_worker (Ssh, NULL);
    }
    WolfSshTcp4Poll (Socket);
    gBS->Stall (5000);
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ConnectWolfSsh (
  IN WOLFSSH                 *Ssh,
  IN OUT WOLFSSH_UEFI_TCP4   *Socket
  )
{
  UINTN Retry;
  INT32 Result;
  INT32 Error;

  for (Retry = 0; Retry < SSH_CONNECT_RETRY_LIMIT; Retry++) {
    Result = wolfSSH_connect (Ssh);
    if (Result == WS_SUCCESS) {
      return EFI_SUCCESS;
    }
    Error = wolfSSH_get_error (Ssh);
    if (!IsRetryableError (Error)) {
      Print (
        L"SSH handshake failed: %d (%a)\r\n",
        Error,
        wolfSSH_ErrorToName (Error)
        );
      return EFI_PROTOCOL_ERROR;
    }
    WolfSshTcp4Poll (Socket);
    gBS->Stall (5000);
  }
  Print (L"SSH handshake timed out.\r\n");
  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
DrainExtendedData (
  IN WOLFSSH                       *Ssh,
  IN OUT WOLFSSH_UEFI_TERMINAL     *Terminal
  )
{
  UINT8 Buffer[1024];
  INT32 Result;

  for (;;) {
    Result = wolfSSH_extended_data_read (Ssh, Buffer, sizeof (Buffer));
    if (Result > 0) {
      WolfSshTerminalFeed (Terminal, Buffer, (UINTN)Result);
      continue;
    }
    if ((Result == 0) || IsRetryableError (wolfSSH_get_error (Ssh))) {
      return EFI_SUCCESS;
    }
    return EFI_DEVICE_ERROR;
  }
}

STATIC
EFI_STATUS
RunInteractiveSession (
  IN WOLFSSH                 *Ssh,
  IN OUT WOLFSSH_UEFI_TCP4   *Socket
  )
{
  WOLFSSH_UEFI_TERMINAL *Terminal;
  UINT8                NetworkBuffer[4096];
  UINT8                KeyBuffer[16];
  UINT8                ReplyBuffer[128];
  UINTN                KeyLength;
  UINTN                ReplyLength;
  UINTN                ReadBurst;
  INT32                Result;
  INT32                Error;
  EFI_STATUS           Status;
  BOOLEAN              LocalExit;
  BOOLEAN              Activity;
  BOOLEAN              Finished;

  Terminal = WolfSshTerminalCreate ();
  if (Terminal == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Finished = FALSE;
  Status = EFI_SUCCESS;
  while (!Finished) {
    Activity = FALSE;
    WolfSshTcp4Poll (Socket);

    for (ReadBurst = 0; ReadBurst < 32; ReadBurst++) {
      Result = wolfSSH_stream_read (Ssh, NetworkBuffer, sizeof (NetworkBuffer));
      if (Result > 0) {
        WolfSshTerminalFeed (Terminal, NetworkBuffer, (UINTN)Result);
        Activity = TRUE;
        continue;
      }

      Error = wolfSSH_get_error (Ssh);
      if ((Result == WS_EXTDATA) || (Error == WS_EXTDATA)) {
        Status = DrainExtendedData (Ssh, Terminal);
        if (EFI_ERROR (Status)) {
          Finished = TRUE;
        }
        Activity = TRUE;
        continue;
      }
      if ((Result == WS_EOF) || (Error == WS_EOF) ||
          (Error == WS_CHANNEL_CLOSED) ||
          (Error == WS_SOCKET_ERROR_E)) {
        Finished = TRUE;
        break;
      }
      if (Error == WS_REKEYING) {
        wolfSSH_worker (Ssh, NULL);
        Activity = TRUE;
        continue;
      }
      if ((Error == WS_WANT_READ) || (Error == WS_WANT_WRITE) ||
          (Result == 0)) {
        break;
      }

      Print (
        L"\r\nSSH receive failed: %d (%a)\r\n",
        Error,
        wolfSSH_ErrorToName (Error)
        );
      Status = EFI_PROTOCOL_ERROR;
      Finished = TRUE;
      break;
    }

    if (Activity) {
      WolfSshTerminalFlush (Terminal);
    }
    do {
      ReplyLength = WolfSshTerminalTakeResponse (
                      Terminal,
                      ReplyBuffer,
                      sizeof (ReplyBuffer)
                      );
      if (ReplyLength != 0) {
        Status = SendAll (Ssh, Socket, ReplyBuffer, ReplyLength);
        if (EFI_ERROR (Status)) {
          Finished = TRUE;
          break;
        }
      }
    } while (ReplyLength != 0);

    while (!Finished) {
      KeyLength = sizeof (KeyBuffer);
      LocalExit = FALSE;
      Status = WolfSshTerminalReadKey (
                 Terminal,
                 KeyBuffer,
                 &KeyLength,
                 &LocalExit
                 );
      if (Status == EFI_NOT_READY) {
        Status = EFI_SUCCESS;
        break;
      }
      if (EFI_ERROR (Status)) {
        Finished = TRUE;
        break;
      }
      if (LocalExit) {
        Finished = TRUE;
        break;
      }
      if (KeyLength != 0) {
        Status = SendAll (Ssh, Socket, KeyBuffer, KeyLength);
        if (EFI_ERROR (Status)) {
          Finished = TRUE;
          break;
        }
        Activity = TRUE;
      }
    }
    if (!Finished && !Activity) {
      gBS->Stall (5000);
    }
  }

  WolfSshTerminalFlush (Terminal);
  WolfSshTerminalDestroy (Terminal);
  Print (L"\r\nRemote terminal closed (exit status %d).\r\n",
    wolfSSH_GetExitStatus (Ssh));
  return Status;
}

STATIC
VOID
BestEffortShutdown (
  IN WOLFSSH                 *Ssh,
  IN OUT WOLFSSH_UEFI_TCP4   *Socket
  )
{
  UINTN Retry;
  INT32 Result;
  INT32 Error;

  for (Retry = 0; Retry < 200; Retry++) {
    Result = wolfSSH_shutdown (Ssh);
    if (Result == WS_SUCCESS) {
      return;
    }
    Error = wolfSSH_get_error (Ssh);
    if (!IsRetryableError (Error)) {
      return;
    }
    WolfSshTcp4Poll (Socket);
    gBS->Stall (5000);
  }
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  WOLFSSH_UEFI_OPTIONS Options;
  WOLFSSH_UEFI_TCP4    Socket;
  WOLFSSH_UEFI_AUTH    Auth;
  WOLFSSH_CTX          *Context;
  WOLFSSH              *Ssh;
  EFI_STATUS           Status;
  INT32                Result;
  UINT8                RandomProbe[16];
  BOOLEAN              WolfSshInitialized;

  (VOID)SystemTable;
  ZeroMem (&Socket, sizeof (Socket));
  ZeroMem (&Auth, sizeof (Auth));
  Context = NULL;
  Ssh = NULL;
  WolfSshInitialized = FALSE;
  WolfCryptLibLinkMarker ();
  WolfSshLibLinkMarker ();

  Status = WolfSshParseArguments (ImageHandle, &Options);
  if (EFI_ERROR (Status)) {
    Print (L"Invalid arguments.\r\n\r\n");
    WolfSshPrintUsage ();
    return EFI_INVALID_PARAMETER;
  }
  if (Options.Help) {
    WolfSshPrintUsage ();
    return EFI_SUCCESS;
  }

  if (UefiRandomGenerateBlock (RandomProbe, sizeof (RandomProbe)) != 0) {
    Print (L"EFI_RNG_PROTOCOL is unavailable; refusing an insecure SSH session.\r\n");
    return EFI_UNSUPPORTED;
  }
  ZeroMem (RandomProbe, sizeof (RandomProbe));

  Result = wolfSSH_Init ();
  if (Result != WS_SUCCESS) {
    Print (L"wolfSSH initialization failed: %d\r\n", Result);
    return EFI_DEVICE_ERROR;
  }
  WolfSshInitialized = TRUE;

  if (Options.SelfTest) {
    Status = WolfSshTerminalSelfTest ();
    goto Cleanup;
  }

  Print (
    L"Connecting to %d.%d.%d.%d:%d...\r\n",
    Options.Address.Addr[0],
    Options.Address.Addr[1],
    Options.Address.Addr[2],
    Options.Address.Addr[3],
    Options.Port
    );
  Status = WolfSshTcp4Connect (
             ImageHandle,
             &Options.Address,
             Options.Port,
             &Socket
             );
  if (EFI_ERROR (Status)) {
    Print (L"TCP4 connection failed: %r\r\n", Status);
    goto Cleanup;
  }

  Context = wolfSSH_CTX_new (WOLFSSH_ENDPOINT_CLIENT, NULL);
  if (Context == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Cleanup;
  }
  wolfSSH_SetIORecv (Context, WolfSshTcp4RecvCallback);
  wolfSSH_SetIOSend (Context, WolfSshTcp4SendCallback);
  wolfSSH_SetUserAuth (Context, WolfSshUserAuthCallback);
  wolfSSH_CTX_SetPublicKeyCheck (Context, WolfSshHostKeyCallback);

  Ssh = wolfSSH_new (Context);
  if (Ssh == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Cleanup;
  }
  Auth.Options = &Options;
  Auth.Socket = &Socket;
  wolfSSH_SetIOReadCtx (Ssh, &Socket);
  wolfSSH_SetIOWriteCtx (Ssh, &Socket);
  wolfSSH_SetUserAuthCtx (Ssh, &Auth);
  wolfSSH_SetPublicKeyCheckCtx (Ssh, &Auth);

  Result = wolfSSH_SetUsername (Ssh, Options.Username);
  if (Result != WS_SUCCESS) {
    Print (L"Invalid SSH username.\r\n");
    Status = EFI_INVALID_PARAMETER;
    goto Cleanup;
  }
  Result = wolfSSH_SetChannelType (
             Ssh,
             WOLFSSH_SESSION_TERMINAL,
             NULL,
             0
             );
  if (Result != WS_SUCCESS) {
    Print (L"Could not request an SSH terminal channel: %d\r\n", Result);
    Status = EFI_PROTOCOL_ERROR;
    goto Cleanup;
  }

  Status = ConnectWolfSsh (Ssh, &Socket);
  if (EFI_ERROR (Status)) {
    goto Cleanup;
  }
  Print (L"SSH terminal connected. Press Ctrl+] to return to UEFI Shell.\r\n");
  Status = RunInteractiveSession (Ssh, &Socket);

Cleanup:
  if (Ssh != NULL) {
    BestEffortShutdown (Ssh, &Socket);
    wolfSSH_free (Ssh);
  }
  if (Context != NULL) {
    wolfSSH_CTX_free (Context);
  }
  if (Socket.Protocol != NULL) {
    WolfSshTcp4Close (&Socket, TRUE);
  }
  ZeroMem (Options.Password, sizeof (Options.Password));
  if (WolfSshInitialized) {
    wolfSSH_Cleanup ();
  }
  return Status;
}
