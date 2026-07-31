#ifndef WOLFSSH_UEFI_APP_H
#define WOLFSSH_UEFI_APP_H

#include <Uefi.h>

#include <Protocol/ServiceBinding.h>
#include <Protocol/Tcp4.h>

#include <wolfssh/ssh.h>

#define WOLFSSH_UEFI_RX_CAPACITY       16384
#define WOLFSSH_UEFI_USERNAME_CAPACITY 64
#define WOLFSSH_UEFI_PASSWORD_CAPACITY 256
#define WOLFSSH_UEFI_FINGERPRINT_SIZE  65

typedef struct {
  EFI_IPv4_ADDRESS Address;
  UINT16           Port;
  CHAR8            Username[WOLFSSH_UEFI_USERNAME_CAPACITY];
  CHAR8            Password[WOLFSSH_UEFI_PASSWORD_CAPACITY];
  CHAR8            Fingerprint[WOLFSSH_UEFI_FINGERPRINT_SIZE];
  BOOLEAN          HavePassword;
  BOOLEAN          HaveFingerprint;
  BOOLEAN          AcceptAnyHostKey;
  BOOLEAN          SelfTest;
  BOOLEAN          Help;
} WOLFSSH_UEFI_OPTIONS;

typedef struct {
  EFI_HANDLE                    Controller;
  EFI_HANDLE                    Child;
  EFI_SERVICE_BINDING_PROTOCOL  *Binding;
  EFI_TCP4_PROTOCOL             *Protocol;
  EFI_EVENT                     RxEvent;
  EFI_EVENT                     TxEvent;
  volatile BOOLEAN              RxDone;
  volatile BOOLEAN              TxDone;
  EFI_TCP4_IO_TOKEN             RxToken;
  EFI_TCP4_IO_TOKEN             TxToken;
  EFI_TCP4_RECEIVE_DATA         RxData;
  EFI_TCP4_TRANSMIT_DATA        TxData;
  UINT8                         RxBuffer[WOLFSSH_UEFI_RX_CAPACITY];
  UINT32                        RxOffset;
  UINT32                        RxLength;
  BOOLEAN                       RxActive;
  BOOLEAN                       EndOfStream;
  EFI_STATUS                    LastRxStatus;
} WOLFSSH_UEFI_TCP4;

typedef struct WOLFSSH_UEFI_TERMINAL WOLFSSH_UEFI_TERMINAL;

typedef struct {
  WOLFSSH_UEFI_OPTIONS  *Options;
  WOLFSSH_UEFI_TCP4     *Socket;
  BOOLEAN               HostKeyChecked;
  BOOLEAN               HostKeyAccepted;
  BOOLEAN               PasswordPrompted;
} WOLFSSH_UEFI_AUTH;

EFI_STATUS
WolfSshParseArguments (
  IN EFI_HANDLE             ImageHandle,
  OUT WOLFSSH_UEFI_OPTIONS  *Options
  );

VOID
WolfSshPrintUsage (
  VOID
  );

EFI_STATUS
WolfSshTcp4Connect (
  IN EFI_HANDLE              ImageHandle,
  IN CONST EFI_IPv4_ADDRESS  *RemoteAddress,
  IN UINT16                  RemotePort,
  OUT WOLFSSH_UEFI_TCP4      *Socket
  );

VOID
WolfSshTcp4Poll (
  IN OUT WOLFSSH_UEFI_TCP4  *Socket
  );

VOID
WolfSshTcp4Close (
  IN OUT WOLFSSH_UEFI_TCP4  *Socket,
  IN BOOLEAN                Abort
  );

int
WolfSshTcp4RecvCallback (
  WOLFSSH      *Ssh,
  void         *Buffer,
  word32       Size,
  void         *Context
  );

int
WolfSshTcp4SendCallback (
  WOLFSSH      *Ssh,
  void         *Buffer,
  word32       Size,
  void         *Context
  );

int
WolfSshUserAuthCallback (
  byte             AuthType,
  WS_UserAuthData  *AuthData,
  void             *Context
  );

int
WolfSshHostKeyCallback (
  const byte  *PublicKey,
  word32      PublicKeySize,
  void        *Context
  );

WOLFSSH_UEFI_TERMINAL *
WolfSshTerminalCreate (
  VOID
  );

VOID
WolfSshTerminalDestroy (
  IN WOLFSSH_UEFI_TERMINAL  *Terminal
  );

VOID
WolfSshTerminalFeed (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal,
  IN CONST UINT8                *Data,
  IN UINTN                      Length
  );

VOID
WolfSshTerminalFlush (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal
  );

UINTN
WolfSshTerminalTakeResponse (
  IN OUT WOLFSSH_UEFI_TERMINAL  *Terminal,
  OUT UINT8                     *Buffer,
  IN UINTN                      Capacity
  );

EFI_STATUS
WolfSshTerminalReadKey (
  IN WOLFSSH_UEFI_TERMINAL  *Terminal,
  OUT UINT8                 *Buffer,
  IN OUT UINTN              *Length,
  OUT BOOLEAN               *LocalExit
  );

EFI_STATUS
WolfSshTerminalSelfTest (
  VOID
  );

#endif
