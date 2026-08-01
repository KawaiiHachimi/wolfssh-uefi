# wolfSSH UEFI Shell Client

[中文](README_zh.md)

This is a working UEFI Shell SSH client prototype for AARCH64 (ARM64) and X64
(x86-64). It links the wolfSSH client, wolfCrypt, and EDK II's native TCP4
protocol directly into an EFI application, with no POSIX, BSD sockets, or
runtime beyond UEFI.

Both architectures have completed real SSH handshakes and bidirectional PTY
tests in QEMU. Rather than executing only a remote command, the client enters
an interactive terminal after connecting, suitable for shells, editors, and
common text TUIs. See the terminal compatibility section for its scope and
known limitations.

![wolfSSH UEFI Shell client](imgs/wolfssh-uefi.png)

## Screenshots

After connecting from UEFI Shell, the client can run common command-line
programs and interactive terminal applications.

### Fastfetch

![Running fastfetch in a wolfSSH UEFI session](imgs/run-fastfetch.png)

### Codex CLI

![Running Codex CLI in a wolfSSH UEFI session](imgs/run-codex.png)

### Kimi Code CLI

![Running Kimi Code CLI in a wolfSSH UEFI session](imgs/run-kimi.png)

## Origin and AI collaboration

The repository owner set the goals, requirements, and acceptance criteria. The
initial implementation was designed, written, built, and validated end-to-end
by ChatGPT (Codex), including the design, wolfSSH/wolfCrypt port, EDK II TCP4
integration, terminal emulator, build scripts, and QEMU end-to-end validation.
This does not constitute official OpenAI support or a security endorsement;
AI-generated code still requires review, physical-firmware testing, and a
security audit.

## Implemented

| Area | Implementation |
|---|---|
| Architectures | AARCH64 and X64, released as `wolfssh-aarch64.efi` and `wolfssh-x64.efi` |
| Networking | Direct `EFI_TCP4_SERVICE_BINDING_PROTOCOL` / `EFI_TCP4_PROTOCOL` use; NIC enumeration, media detection, asynchronous receive, and timed send/close |
| Addressing | Uses the same TCP4 approach as `iperf3-uefi`: `UseDefaultAddress=TRUE`, waits for `EFI_NO_MAPPING` to clear, and uses the firmware's existing DHCP or static IPv4 mapping |
| SSH | wolfSSH client, password authentication, PTY, and interactive shell channel |
| Randomness | Requires `EFI_RNG_PROTOCOL`; an SSH session is refused when it is unavailable |
| Host keys | Shows a SHA-256 hexadecimal fingerprint; supports interactive confirmation, exact `-f` pinning, and one-time insecure `-y` acceptance |
| Terminal output | UTF-8 (BMP), cursor motion/positioning, screen and line erase, scrolling regions, insert/delete lines and characters, saved cursor, 16-color and 256/RGB-to-16-color approximation, primary/alternate screens, and cursor visibility |
| Bidirectional terminal | DSR/device-status replies; arrows, Home/End, Insert/Delete, PageUp/PageDown, F1–F10, and application cursor mode |
| Local exit | `Ctrl+]` returns to UEFI Shell |

The current cryptographic interoperability configuration is:

- KEX: `ecdh-sha2-nistp256`
- Host key: `ecdsa-sha2-nistp256`
- Encryption: AES-128/192/256 GCM and CTR
- MAC: `hmac-sha2-256` (AEAD for GCM)
- User authentication: password

This set works with ordinary OpenSSH servers which retain an ECDSA P-256 host
key. Custom servers offering only Ed25519, RSA, or Curve25519 cannot currently
negotiate with the client.

## Quick build

On x86-64 Linux, first retrieve the pinned source revisions. `ARCH=ALL`
prepares the AARCH64 toolchain, UEFI Shell for both architectures, and the
Python package used for testing:

```bash
./scripts/fetch-deps.sh
ARCH=ALL ./scripts/fetch-tools.sh

ARCH=AARCH64 BUILD_TARGET=RELEASE ./scripts/build.sh
ARCH=X64 BUILD_TARGET=RELEASE ./scripts/build.sh
```

For an X64-only build, use `ARCH=X64 ./scripts/fetch-tools.sh`. The output
files are:

```text
.build/output/wolfssh-aarch64.efi
.build/output/wolfssh-x64.efi
```

Each build also copies the result to `.build/output/wolfssh.efi` for existing
automation. Prefer the architecture-suffixed file when publishing or copying
to an ESP.

Source and tool revisions, download locations, and SHA-256 checksums are all
recorded in `deps.lock`. The default layout is:

```text
parent/
├── wolfssh-uefi/
├── upstream/
└── toolchains/
```

The build script runs EDK II `GenFw -z` on the final copy to remove absolute
build paths and timestamp fields from the PE debug directory. AARCH64 uses the
pinned cross compiler; X64 uses the host GCC/binutils/NASM, so reproducing an
X64 binary across hosts also requires matching their versions.

If you already have EDK II or a toolchain installed, set:

```bash
EDK2_ROOT=/path/to/edk2 \
AARCH64_TOOLCHAIN_ROOT=/path/to/xpack-aarch64-none-elf-gcc-15.2.1-1.1 \
ARCH=AARCH64 BUILD_TARGET=RELEASE ./scripts/build.sh

EDK2_ROOT=/path/to/edk2 \
ARCH=X64 BUILD_TARGET=RELEASE ./scripts/build.sh
```

The host build requires at least Bash, Git, Python 3, GNU Make, a host C
compiler, `curl`, `tar`, `sha256sum`, and the UUID development library needed
by EDK II BaseTools. X64 additionally requires host GCC, GNU binutils, and
NASM; X64 QEMU testing requires `qemu-system-x86_64` and OVMF. The pinned
AARCH64 prebuilt toolchain targets Linux x86-64; on other hosts, prepare an
equivalent toolchain manually and use the environment variables above.

## Use in UEFI Shell

Copy the EFI file matching the firmware architecture to an ESP or USB drive.
Typical PCs use X64 and ARM64 devices use AARCH64; UEFI cannot execute an EFI
file built for the other architecture. Keep the release name or rename it to
`wolfssh.efi`, then run it from UEFI Shell:

```text
fs0:\wolfssh.efi user@192.0.2.10
```

For a non-default port:

```text
fs0:\wolfssh.efi -p 2222 user@192.0.2.10
```

The first connection displays the host-key fingerprint and asks for
confirmation. For regular use, calculate the fingerprint from a trusted server
side:

```bash
awk '{print $2}' /etc/ssh/ssh_host_ecdsa_key.pub | base64 -d | sha256sum
```

Then pin it:

```text
fs0:\wolfssh.efi -f 64-hex-character-SHA256 user@192.0.2.10
```

Without `-P`, password entry is not echoed. `-P password` is only suitable for
automated tests because the password appears in UEFI Shell arguments and may
appear in command history. `-y` is likewise for testing only: it accepts an
unpinned host key only for the current connection.

The client uses the firmware's default IPv4 mapping. On physical hardware, the
NIC, SNP/MNP, IPv4, and TCP4 drivers must already be loaded, and the firmware
must obtain an address through DHCP or static configuration. If the shell
includes `ifconfig`, use it to check the interface first. The application does
not implement a NIC driver or DHCP client; it uses the firmware network stack.

Run the built-in terminal check with:

```text
fs0:\wolfssh.efi --self-test
```

## QEMU end-to-end testing

```bash
ARCH=AARCH64 ./scripts/test-qemu.sh
ARCH=X64 ./scripts/test-qemu.sh
```

The test script:

1. Builds the selected architecture's RELEASE EFI application.
2. Generates a one-time ECDSA P-256 host key and starts an AsyncSSH server
   restricted to the target algorithms and test password.
3. Provides DHCP, ARP, and single-connection TCP forwarding through an
   unprivileged QEMU stream backend.
4. Starts QEMU with the matching EDK II firmware and UEFI Shell.
5. Verifies exact `-f` pinning of the temporary host key, password
   authentication, PTY size, shell channel, colors, erasing, cursor placement,
   and bidirectional DSR replies.
6. Requires the remote session to close with status 0.

The AARCH64 test uses the QEMU and firmware downloaded and verified by the
script. The X64 test uses the host-installed QEMU and OVMF. Logs are written to
`.build/test-results/aarch64/` and `.build/test-results/x64/`, respectively.

This network proxy exists solely for repeatable testing; it is not a general
user-space networking stack. On actual firmware, `Tcp4.c` uses EDK II's TCP4
path.

See `QEMU_VALIDATION.md` for validated versions, binary checksums, and test
markers.

## Terminal compatibility boundaries

Screen clearing works: remote `ESC[2J` is parsed into UEFI
`EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL.ClearScreen()`, while other common ANSI/VT
sequences are mapped to UEFI cursor, color, and screen-buffer operations.

This is not a complete xterm implementation:

- Mouse, clipboard, sixel graphics, and bracketed paste are unsupported.
- The console size is not reported dynamically during a session.
- 24-bit and 256-color values are approximated to UEFI's 16 colors.
- Non-BMP Unicode displays as `?`; CJK and combining characters occupy one cell.
- OSC title and similar sequences are safely ignored.
- Only IPv4 literals are supported; there is no DNS or IPv6.
- There is no persistent `known_hosts`; after a reboot, confirm again or use `-f`.
- Neither AARCH64 nor X64 EFI file is signed; Secure Boot may refuse to load it.

Ordinary shells, `top`/`htop`, `vim`/`nano`, and menu-driven TUIs therefore have
a usable interaction foundation. Programs which depend heavily on a complete
xterm, mouse support, or precise Unicode width may still render incompletely.
The project has not undergone a security audit and should not be used directly
for irreversible production maintenance.

## Layout

```text
WolfSshPkg/Application/WolfSsh/  CLI, authentication, TCP4, session loop, terminal emulator
WolfSshPkg/Library/              UEFI libc compatibility layer, wolfCrypt/wolfSSH EDK II libraries
WolfSshPkg/Include/              wolfSSL user configuration and library interfaces
patches/                         wolfSSH patch enabling UEFI PTY without a filesystem
scripts/                         dependency, tool, build, and QEMU regression scripts
tests/                           deterministic SSH server and QEMU test network proxy
```

## Acknowledgments

- [ChatGPT (Codex)](https://openai.com/chatgpt/overview/): designed, implemented, built, and validated the project end-to-end.
- [wolfSSH](https://github.com/wolfSSL/wolfssh) and [wolfSSL / wolfCrypt](https://github.com/wolfSSL/wolfssl): provide the SSH client and cryptographic implementation.
- [tianocore/edk2](https://github.com/tianocore/edk2): provides the UEFI build infrastructure, libraries, and networking protocol interfaces.
- [BigfootACA/iperf3-uefi](https://github.com/BigfootACA/iperf3-uefi): provided design reference for the TCP4 lifecycle and default IPv4 mapping.
- [QEMU](https://www.qemu.org/), [OVMF](https://github.com/tianocore/edk2/tree/master/OvmfPkg), and [AsyncSSH](https://github.com/ronf/asyncssh): support end-to-end validation.

## License

The default build links the GPLv3 editions of wolfSSH/wolfSSL, so this release
is provided under GPLv3. wolfSSH and wolfSSL also offer commercial licenses;
for a commercial-license route, confirm the terms separately with wolfSSL Inc.
See `LICENSE` and `THIRD_PARTY_NOTICES.md`.
