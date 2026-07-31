#include "WolfSshUefi.h"

#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/NetLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <wolfssh/error.h>

#define WOLFSSH_TCP_CONNECT_TIMEOUT_US  30000000ULL
#define WOLFSSH_TCP_IO_TIMEOUT_US       30000000ULL
#define WOLFSSH_TCP_CANCEL_TIMEOUT_US    1000000ULL

STATIC
UINT64
NowMicroseconds (
  VOID
  )
{
  return DivU64x32 (GetTimeInNanoSecond (GetPerformanceCounter ()), 1000);
}

STATIC
VOID
EFIAPI
NetworkNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  volatile BOOLEAN *Done;

  (VOID)Event;
  Done = (volatile BOOLEAN *)Context;
  *Done = TRUE;
}

STATIC
EFI_STATUS
WaitForCompletion (
  IN EFI_TCP4_PROTOCOL  *Protocol,
  IN volatile BOOLEAN   *Done,
  IN UINT64             TimeoutMicroseconds
  )
{
  UINT64 Deadline;

  Deadline = NowMicroseconds () + TimeoutMicroseconds;
  while (!*Done) {
    Protocol->Poll (Protocol);
    if (*Done) {
      break;
    }
    if (NowMicroseconds () >= Deadline) {
      return EFI_TIMEOUT;
    }
    gBS->Stall (50);
  }
  return EFI_SUCCESS;
}

STATIC
VOID
CancelToken (
  IN OUT WOLFSSH_UEFI_TCP4       *Socket,
  IN EFI_TCP4_COMPLETION_TOKEN   *Token,
  IN volatile BOOLEAN            *Done
  )
{
  UINT64 Deadline;

  Socket->Protocol->Cancel (Socket->Protocol, Token);
  Deadline = NowMicroseconds () + WOLFSSH_TCP_CANCEL_TIMEOUT_US;
  while (!*Done && (NowMicroseconds () < Deadline)) {
    Socket->Protocol->Poll (Socket->Protocol);
    gBS->Stall (50);
  }
}

STATIC
EFI_STATUS
PostReceive (
  IN OUT WOLFSSH_UEFI_TCP4  *Socket
  )
{
  EFI_STATUS Status;

  if (Socket->RxActive || Socket->EndOfStream) {
    return EFI_SUCCESS;
  }

  ZeroMem (&Socket->RxToken, sizeof (Socket->RxToken));
  ZeroMem (&Socket->RxData, sizeof (Socket->RxData));
  Socket->RxDone = FALSE;
  Socket->RxOffset = 0;
  Socket->RxLength = 0;
  Socket->RxData.DataLength = sizeof (Socket->RxBuffer);
  Socket->RxData.FragmentCount = 1;
  Socket->RxData.FragmentTable[0].FragmentLength = sizeof (Socket->RxBuffer);
  Socket->RxData.FragmentTable[0].FragmentBuffer = Socket->RxBuffer;
  Socket->RxToken.CompletionToken.Event = Socket->RxEvent;
  Socket->RxToken.CompletionToken.Status = EFI_NOT_READY;
  Socket->RxToken.Packet.RxData = &Socket->RxData;

  Status = Socket->Protocol->Receive (Socket->Protocol, &Socket->RxToken);
  if (!EFI_ERROR (Status)) {
    Socket->RxActive = TRUE;
  }
  return Status;
}

STATIC
VOID
FinishReceive (
  IN OUT WOLFSSH_UEFI_TCP4  *Socket
  )
{
  if (!Socket->RxActive || !Socket->RxDone) {
    return;
  }

  Socket->RxActive = FALSE;
  Socket->LastRxStatus = Socket->RxToken.CompletionToken.Status;
  if (Socket->LastRxStatus == EFI_CONNECTION_FIN) {
    Socket->EndOfStream = TRUE;
    return;
  }
  if (EFI_ERROR (Socket->LastRxStatus)) {
    return;
  }
  Socket->RxOffset = 0;
  Socket->RxLength = Socket->RxData.DataLength;
}

VOID
WolfSshTcp4Poll (
  IN OUT WOLFSSH_UEFI_TCP4  *Socket
  )
{
  if ((Socket == NULL) || (Socket->Protocol == NULL)) {
    return;
  }
  Socket->Protocol->Poll (Socket->Protocol);
  FinishReceive (Socket);
}

STATIC
EFI_STATUS
OpenOnController (
  IN EFI_HANDLE              Controller,
  IN CONST EFI_IPv4_ADDRESS  *RemoteAddress,
  IN UINT16                  RemotePort,
  OUT WOLFSSH_UEFI_TCP4      *Socket
  )
{
  EFI_TCP4_CONFIG_DATA      Config;
  EFI_TCP4_OPTION           Option;
  EFI_TCP4_CONNECTION_TOKEN ConnectToken;
  EFI_EVENT                 ConnectEvent;
  volatile BOOLEAN          ConnectDone;
  EFI_STATUS                Status;
  UINTN                     Retry;

  ZeroMem (Socket, sizeof (*Socket));
  Socket->Controller = Controller;
  Socket->LastRxStatus = EFI_NOT_READY;

  Status = gBS->HandleProtocol (
                  Controller,
                  &gEfiTcp4ServiceBindingProtocolGuid,
                  (VOID **)&Socket->Binding
                  );
  if (EFI_ERROR (Status)) {
    goto Error;
  }
  Status = Socket->Binding->CreateChild (Socket->Binding, &Socket->Child);
  if (EFI_ERROR (Status)) {
    goto Error;
  }
  Status = gBS->HandleProtocol (
                  Socket->Child,
                  &gEfiTcp4ProtocolGuid,
                  (VOID **)&Socket->Protocol
                  );
  if (EFI_ERROR (Status)) {
    goto Error;
  }
  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  NetworkNotify,
                  (VOID *)&Socket->RxDone,
                  &Socket->RxEvent
                  );
  if (EFI_ERROR (Status)) {
    goto Error;
  }
  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  NetworkNotify,
                  (VOID *)&Socket->TxDone,
                  &Socket->TxEvent
                  );
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  ZeroMem (&Config, sizeof (Config));
  ZeroMem (&Option, sizeof (Option));
  Config.AccessPoint.UseDefaultAddress = TRUE;
  Config.AccessPoint.RemoteAddress = *RemoteAddress;
  Config.AccessPoint.RemotePort = RemotePort;
  Config.AccessPoint.ActiveFlag = TRUE;
  Option.ReceiveBufferSize = 64 * 1024;
  Option.SendBufferSize = 64 * 1024;
  Option.EnableNagle = FALSE;
  Option.EnableWindowScaling = TRUE;
  Option.EnableSelectiveAck = FALSE;
  Option.EnablePathMtuDiscovery = FALSE;
  Config.ControlOption = &Option;

  for (Retry = 0; Retry < 100; Retry++) {
    Status = Socket->Protocol->Configure (Socket->Protocol, &Config);
    if (Status != EFI_NO_MAPPING) {
      break;
    }
    gBS->Stall (100000);
  }
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  ConnectEvent = NULL;
  ConnectDone = FALSE;
  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  NetworkNotify,
                  (VOID *)&ConnectDone,
                  &ConnectEvent
                  );
  if (EFI_ERROR (Status)) {
    goto Error;
  }
  ZeroMem (&ConnectToken, sizeof (ConnectToken));
  ConnectToken.CompletionToken.Event = ConnectEvent;
  ConnectToken.CompletionToken.Status = EFI_NOT_READY;
  Status = Socket->Protocol->Connect (Socket->Protocol, &ConnectToken);
  if (!EFI_ERROR (Status)) {
    Status = WaitForCompletion (
               Socket->Protocol,
               &ConnectDone,
               WOLFSSH_TCP_CONNECT_TIMEOUT_US
               );
  }
  if (Status == EFI_TIMEOUT) {
    CancelToken (Socket, &ConnectToken.CompletionToken, &ConnectDone);
  }
  if (!EFI_ERROR (Status)) {
    Status = ConnectToken.CompletionToken.Status;
  }
  gBS->CloseEvent (ConnectEvent);
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  Status = PostReceive (Socket);
  if (EFI_ERROR (Status)) {
    goto Error;
  }
  return EFI_SUCCESS;

Error:
  WolfSshTcp4Close (Socket, TRUE);
  return Status;
}

EFI_STATUS
WolfSshTcp4Connect (
  IN EFI_HANDLE              ImageHandle,
  IN CONST EFI_IPv4_ADDRESS  *RemoteAddress,
  IN UINT16                  RemotePort,
  OUT WOLFSSH_UEFI_TCP4      *Socket
  )
{
  EFI_HANDLE *Handles;
  EFI_STATUS Status;
  EFI_STATUS MediaState;
  UINTN      Count;
  UINTN      Index;

  (VOID)ImageHandle;
  if ((RemoteAddress == NULL) || (RemotePort == 0) || (Socket == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Handles = NULL;
  Count = 0;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiTcp4ServiceBindingProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = EFI_NOT_FOUND;
  for (Index = 0; Index < Count; Index++) {
    if (!EFI_ERROR (
           NetLibDetectMediaWaitTimeout (Handles[Index], 0, &MediaState)
           ) && (MediaState != EFI_SUCCESS)) {
      Status = MediaState;
      continue;
    }
    Status = OpenOnController (
               Handles[Index],
               RemoteAddress,
               RemotePort,
               Socket
               );
    if (!EFI_ERROR (Status)) {
      break;
    }
  }
  FreePool (Handles);
  return Status;
}

int
WolfSshTcp4RecvCallback (
  WOLFSSH  *Ssh,
  void     *Buffer,
  word32   Size,
  void     *Context
  )
{
  WOLFSSH_UEFI_TCP4 *Socket;
  UINT32            Available;
  UINT32            CopyLength;
  EFI_STATUS        Status;

  (VOID)Ssh;
  Socket = (WOLFSSH_UEFI_TCP4 *)Context;
  if ((Socket == NULL) || (Buffer == NULL) || (Size == 0)) {
    return WS_CBIO_ERR_GENERAL;
  }

  WolfSshTcp4Poll (Socket);
  if (Socket->RxLength > Socket->RxOffset) {
    Available = Socket->RxLength - Socket->RxOffset;
    CopyLength = MIN ((UINT32)Size, Available);
    CopyMem (Buffer, Socket->RxBuffer + Socket->RxOffset, CopyLength);
    Socket->RxOffset += CopyLength;
    if (Socket->RxOffset == Socket->RxLength) {
      Socket->RxOffset = 0;
      Socket->RxLength = 0;
      Status = PostReceive (Socket);
      if (EFI_ERROR (Status)) {
        Socket->LastRxStatus = Status;
      }
    }
    return (int)CopyLength;
  }

  if (Socket->EndOfStream) {
    return WS_CBIO_ERR_CONN_CLOSE;
  }
  if (EFI_ERROR (Socket->LastRxStatus) &&
      (Socket->LastRxStatus != EFI_NOT_READY)) {
    return (Socket->LastRxStatus == EFI_CONNECTION_RESET) ?
           WS_CBIO_ERR_CONN_RST : WS_CBIO_ERR_GENERAL;
  }
  if (!Socket->RxActive) {
    Status = PostReceive (Socket);
    if (EFI_ERROR (Status)) {
      return WS_CBIO_ERR_GENERAL;
    }
  }
  return WS_CBIO_ERR_WANT_READ;
}

int
WolfSshTcp4SendCallback (
  WOLFSSH  *Ssh,
  void     *Buffer,
  word32   Size,
  void     *Context
  )
{
  WOLFSSH_UEFI_TCP4 *Socket;
  EFI_STATUS        Status;

  (VOID)Ssh;
  Socket = (WOLFSSH_UEFI_TCP4 *)Context;
  if ((Socket == NULL) || (Socket->Protocol == NULL) ||
      (Buffer == NULL) || (Size == 0)) {
    return WS_CBIO_ERR_GENERAL;
  }

  ZeroMem (&Socket->TxToken, sizeof (Socket->TxToken));
  ZeroMem (&Socket->TxData, sizeof (Socket->TxData));
  Socket->TxDone = FALSE;
  Socket->TxData.Push = TRUE;
  Socket->TxData.DataLength = Size;
  Socket->TxData.FragmentCount = 1;
  Socket->TxData.FragmentTable[0].FragmentLength = Size;
  Socket->TxData.FragmentTable[0].FragmentBuffer = Buffer;
  Socket->TxToken.CompletionToken.Event = Socket->TxEvent;
  Socket->TxToken.CompletionToken.Status = EFI_NOT_READY;
  Socket->TxToken.Packet.TxData = &Socket->TxData;

  Status = Socket->Protocol->Transmit (Socket->Protocol, &Socket->TxToken);
  if (EFI_ERROR (Status)) {
    return WS_CBIO_ERR_GENERAL;
  }
  Status = WaitForCompletion (
             Socket->Protocol,
             &Socket->TxDone,
             WOLFSSH_TCP_IO_TIMEOUT_US
             );
  if (Status == EFI_TIMEOUT) {
    CancelToken (Socket, &Socket->TxToken.CompletionToken, &Socket->TxDone);
    return WS_CBIO_ERR_TIMEOUT;
  }
  Status = Socket->TxToken.CompletionToken.Status;
  if (Status == EFI_CONNECTION_RESET) {
    return WS_CBIO_ERR_CONN_RST;
  }
  if (EFI_ERROR (Status)) {
    return WS_CBIO_ERR_GENERAL;
  }
  return (int)Size;
}

VOID
WolfSshTcp4Close (
  IN OUT WOLFSSH_UEFI_TCP4  *Socket,
  IN BOOLEAN                Abort
  )
{
  EFI_TCP4_CLOSE_TOKEN CloseToken;
  EFI_STATUS           Status;

  if (Socket == NULL) {
    return;
  }
  if ((Socket->Protocol != NULL) && Socket->RxActive && !Socket->RxDone) {
    CancelToken (
      Socket,
      &Socket->RxToken.CompletionToken,
      &Socket->RxDone
      );
    Socket->RxActive = FALSE;
  }
  if ((Socket->Protocol != NULL) && (Socket->TxEvent != NULL)) {
    ZeroMem (&CloseToken, sizeof (CloseToken));
    Socket->TxDone = FALSE;
    CloseToken.CompletionToken.Event = Socket->TxEvent;
    CloseToken.CompletionToken.Status = EFI_NOT_READY;
    CloseToken.AbortOnClose = Abort;
    Status = Socket->Protocol->Close (Socket->Protocol, &CloseToken);
    if (!EFI_ERROR (Status)) {
      WaitForCompletion (
        Socket->Protocol,
        &Socket->TxDone,
        2000000
        );
    }
    Socket->Protocol->Configure (Socket->Protocol, NULL);
  }
  if ((Socket->Binding != NULL) && (Socket->Child != NULL)) {
    Socket->Binding->DestroyChild (Socket->Binding, Socket->Child);
  }
  if (Socket->RxEvent != NULL) {
    gBS->CloseEvent (Socket->RxEvent);
  }
  if (Socket->TxEvent != NULL) {
    gBS->CloseEvent (Socket->TxEvent);
  }
  ZeroMem (Socket, sizeof (*Socket));
}
