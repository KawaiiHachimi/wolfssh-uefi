#!/usr/bin/env python3
"""SSH/PTTY peer used by the QEMU end-to-end test."""

import argparse
import asyncio
import sys
from pathlib import Path

import asyncssh


class TestServer(asyncssh.SSHServer):
    def connection_lost(self, exc):
        print(f"SERVER_CONNECTION_LOST error={exc!r}", flush=True)

    def begin_auth(self, username):
        print(f"AUTH_BEGIN user={username}", flush=True)
        return True

    def password_auth_supported(self):
        return True

    def validate_password(self, username, password):
        accepted = username == "test" and password == "test"
        print(f"AUTH_PASSWORD accepted={accepted}", flush=True)
        return accepted

    def session_requested(self):
        print("SESSION_REQUEST accepted=True", flush=True)
        return TestSession()


class TestSession(asyncssh.SSHServerSession):
    def __init__(self):
        self.channel = None
        self.pty_ok = False
        self.received = ""
        self.timeout_handle = None

    def connection_made(self, channel):
        self.channel = channel

    def pty_requested(self, term_type, term_size, term_modes):
        columns, rows, pixel_width, pixel_height = term_size
        self.pty_ok = (
            term_type.startswith("xterm") and columns > 0 and rows > 0
        )
        print(
            "PTY_REQUEST "
            f"term={term_type} columns={columns} rows={rows} "
            f"pixels={pixel_width}x{pixel_height} accepted={self.pty_ok}",
            flush=True,
        )
        return self.pty_ok

    def shell_requested(self):
        print(f"SHELL_REQUEST accepted={self.pty_ok}", flush=True)
        return self.pty_ok

    def session_started(self):
        loop = asyncio.get_running_loop()
        loop.call_later(0.15, self._send_terminal_probe)
        self.timeout_handle = loop.call_later(8.0, self._finish, 1)

    def _send_terminal_probe(self):
        if self.channel is None:
            return
        self.channel.write(
            "\x1b[2J\x1b[H"
            "\x1b[1;32mUEFI_WOLFSSH_COLOR_OK\x1b[0m\r\n"
            "erase-this\x1b[2K\rUEFI_WOLFSSH_ERASE_OK\r\n"
            "\x1b[10;20HUEFI_WOLFSSH_CURSOR_OK\r\n"
            "\x1b[?1049hUEFI_WOLFSSH_ALTSCREEN_OK\x1b[?1049l"
            "\x1b[12;20H\r\x1b[6n"
        )
        print("TERMINAL_PROBE_SENT", flush=True)

    def data_received(self, data, datatype):
        if isinstance(data, bytes):
            data = data.decode("latin-1")
        self.received += data
        print(f"TERMINAL_INPUT data={data!r}", flush=True)
        if "\x1b[12;1R" in self.received:
            if self.timeout_handle is not None:
                self.timeout_handle.cancel()
                self.timeout_handle = None
            self.channel.write("\x1b[12;1HUEFI_WOLFSSH_E2E_OK\r\n")
            print("TERMINAL_DSR_OK row=12 column=1", flush=True)
            asyncio.get_running_loop().call_later(0.25, self._finish, 0)

    def _finish(self, exit_status):
        if self.channel is not None:
            self.channel.exit(exit_status)


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host-key", required=True, type=Path)
    return parser.parse_args()


async def main(host_key):
    key = asyncssh.read_private_key(host_key)
    server = await asyncssh.create_server(
        TestServer,
        "127.0.0.1",
        2222,
        server_host_keys=[key],
        kex_algs=["ecdh-sha2-nistp256"],
        encryption_algs=["aes128-gcm@openssh.com", "aes128-ctr"],
        mac_algs=["hmac-sha2-256"],
        line_editor=False,
        line_echo=False,
    )
    print("SSH_TEST_SERVER_READY port=2222", flush=True)
    async with server:
        await server.wait_closed()


if __name__ == "__main__":
    args = parse_args()
    try:
        asyncio.run(main(args.host_key))
    except (OSError, asyncssh.Error) as exc:
        print(f"SSH_TEST_SERVER_ERROR {exc}", file=sys.stderr, flush=True)
        raise SystemExit(1)
