#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "$0")/.." && pwd)"
tool_root="${TOOL_ROOT:-$project_root/../toolchains}"
test_pythonpath="${TEST_PYTHONPATH:-$tool_root/python}"
build_target="${BUILD_TARGET:-RELEASE}"
build_directory="$project_root/.build"
arch="${ARCH:-AARCH64}"

first_existing_file() {
  local candidate

  for candidate in "$@"; do
    if [[ -f "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

case "$arch" in
  AARCH64)
    artifact_arch="aarch64"
    boot_filename="BOOTAA64.EFI"
    qemu_root="${QEMU_ROOT:-$tool_root/installed/xpack-qemu-arm-9.2.4-1}"
    qemu_binary="${QEMU_AARCH64:-$qemu_root/bin/qemu-system-aarch64}"
    qemu_code="${QEMU_CODE_FD:-$qemu_root/share/qemu/edk2-aarch64-code.fd}"
    qemu_vars_template="${QEMU_VARS_FD:-$qemu_root/share/qemu/edk2-arm-vars.fd}"
    shell_binary="${SHELLAA64_EFI:-$tool_root/downloads/shellaa64-26H1.efi}"
    qemu_platform_args=(-machine virt -cpu cortex-a57)
    ;;
  X64)
    artifact_arch="x64"
    boot_filename="BOOTX64.EFI"
    qemu_binary="${QEMU_X64:-$(command -v qemu-system-x86_64 || true)}"
    qemu_code="${QEMU_CODE_FD:-$(first_existing_file \
      /usr/share/OVMF/OVMF_CODE_4M.fd \
      /usr/share/OVMF/OVMF_CODE.fd \
      /usr/share/edk2/ovmf/OVMF_CODE.fd \
      /usr/share/edk2/x64/OVMF_CODE.fd \
      /usr/share/qemu/OVMF_CODE.fd || true)}"
    if [[ "$qemu_code" == *4M* ]]; then
      qemu_vars_template="${QEMU_VARS_FD:-$(first_existing_file \
        /usr/share/OVMF/OVMF_VARS_4M.fd \
        /usr/share/edk2/ovmf/OVMF_VARS_4M.fd || true)}"
    else
      qemu_vars_template="${QEMU_VARS_FD:-$(first_existing_file \
        /usr/share/OVMF/OVMF_VARS.fd \
        /usr/share/edk2/ovmf/OVMF_VARS.fd \
        /usr/share/edk2/x64/OVMF_VARS.fd \
        /usr/share/qemu/OVMF_VARS.fd || true)}"
    fi
    shell_binary="${SHELLX64_EFI:-$tool_root/downloads/shellx64-26H1.efi}"
    qemu_platform_args=(-machine q35 -cpu max -vga none)
    ;;
  *)
    echo "Unsupported ARCH=$arch; expected AARCH64 or X64" >&2
    exit 2
    ;;
esac

esp_directory="$build_directory/qemu/$artifact_arch/esp"
qemu_state_directory="$build_directory/qemu/$artifact_arch"
log_directory="$build_directory/test-results/$artifact_arch"
application="$build_directory/output/wolfssh-$artifact_arch.efi"
qemu_data_args=()
if [[ -n "${QEMU_DATA_DIR:-}" ]]; then
  qemu_data_args=(-L "$QEMU_DATA_DIR")
fi

for required in "$qemu_code" "$qemu_vars_template" "$shell_binary"; do
  if [[ ! -f "$required" ]]; then
    echo "Missing QEMU test dependency: $required" >&2
    echo "Run ARCH=$arch ./scripts/fetch-tools.sh or set the matching environment variable." >&2
    exit 2
  fi
done
if [[ -z "$qemu_binary" || ! -x "$qemu_binary" ]]; then
  echo "Missing QEMU executable for ARCH=$arch: $qemu_binary" >&2
  echo "X64 requires qemu-system-x86_64 and OVMF from the host distribution." >&2
  exit 2
fi
if ! PYTHONPATH="$test_pythonpath" python3 -c 'import asyncssh' 2>/dev/null; then
  echo "AsyncSSH is unavailable; run scripts/fetch-tools.sh" >&2
  exit 2
fi

if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
  ARCH="$arch" BUILD_TARGET="$build_target" "$project_root/scripts/build.sh"
fi
if [[ ! -f "$application" ]]; then
  echo "Missing built application: $application" >&2
  exit 2
fi

mkdir -p "$esp_directory/EFI/BOOT" "$qemu_state_directory" "$log_directory"
cp "$shell_binary" "$esp_directory/EFI/BOOT/$boot_filename"
cp "$application" "$esp_directory/wolfssh.efi"
cp "$qemu_vars_template" "$qemu_state_directory/vars.fd"

host_key="$qemu_state_directory/ssh_host_ecdsa_key"
host_key_fingerprint="$(
  PYTHONPATH="$test_pythonpath" python3 - "$host_key" <<'PY'
import base64
import hashlib
import sys
from pathlib import Path

import asyncssh

key_path = Path(sys.argv[1])
key = asyncssh.generate_private_key("ecdsa-sha2-nistp256")
key_path.write_bytes(key.export_private_key("openssh"))
key_path.chmod(0o600)
public_key = key.export_public_key("openssh").split()
print(hashlib.sha256(base64.b64decode(public_key[1])).hexdigest())
PY
)"
if [[ ! "$host_key_fingerprint" =~ ^[0-9a-f]{64}$ ]]; then
  echo "Failed to generate the temporary SSH host-key fingerprint" >&2
  exit 1
fi
sed "s/@HOST_KEY_FINGERPRINT@/$host_key_fingerprint/" \
  "$project_root/tests/qemu-startup.nsh" >"$esp_directory/startup.nsh"

server_log="$log_directory/ssh-server.log"
proxy_log="$log_directory/net-proxy.log"
qemu_log="$log_directory/qemu-e2e.log"

PYTHONPATH="$test_pythonpath" python3 "$project_root/tests/ssh_test_server.py" \
  --host-key "$host_key" \
  >"$server_log" 2>&1 &
server_pid=$!
python3 "$project_root/tests/qemu_net_proxy.py" --listen-port 3333 --port 2222 \
  >"$proxy_log" 2>&1 &
proxy_pid=$!
cleanup() {
  kill "$server_pid" "$proxy_pid" 2>/dev/null || true
  wait "$server_pid" "$proxy_pid" 2>/dev/null || true
}
trap cleanup EXIT

ready=0
for _attempt in $(seq 1 50); do
  if rg -q 'SSH_TEST_SERVER_READY' "$server_log" &&
     rg -q 'QEMU_NET_PROXY_READY' "$proxy_log"; then
    ready=1
    break
  fi
  if ! kill -0 "$server_pid" 2>/dev/null || ! kill -0 "$proxy_pid" 2>/dev/null; then
    break
  fi
  sleep 0.1
done
if [[ "$ready" != "1" ]]; then
  echo "QEMU test services failed to start" >&2
  sed -n '1,120p' "$server_log" >&2
  sed -n '1,120p' "$proxy_log" >&2
  exit 1
fi

set +e
timeout "${QEMU_TIMEOUT_SECONDS:-45}" "$qemu_binary" \
  "${qemu_data_args[@]}" "${qemu_platform_args[@]}" \
  -m 512 -nographic -no-reboot \
  -drive if=pflash,format=raw,unit=0,readonly=on,file="$qemu_code" \
  -drive if=pflash,format=raw,unit=1,file="$qemu_state_directory/vars.fd" \
  -drive if=none,format=raw,id=esp,readonly=on,file="fat:ro:$esp_directory" \
  -device virtio-blk-pci,drive=esp,romfile= \
  -netdev stream,id=net0,server=off,addr.type=inet,addr.host=127.0.0.1,addr.port=3333 \
  -device virtio-net-pci,netdev=net0,mac=52:54:00:ab:cd:ef,romfile= \
  -object rng-random,id=rng0,filename=/dev/urandom \
  -device virtio-rng-pci,rng=rng0 >"$qemu_log" 2>&1
qemu_status=$?
set -e
if [[ "$qemu_status" != "0" ]]; then
  echo "QEMU exited with status $qemu_status" >&2
  tail -120 "$qemu_log" >&2
  exit 1
fi

rg -a -q 'UEFI_WOLFSSH_COLOR_OK' "$qemu_log"
rg -a -q 'UEFI_WOLFSSH_ERASE_OK' "$qemu_log"
rg -a -q 'UEFI_WOLFSSH_CURSOR_OK' "$qemu_log"
rg -a -q 'UEFI_WOLFSSH_E2E_OK' "$qemu_log"
rg -a -F -q "Server host-key SHA-256: $host_key_fingerprint" "$qemu_log"
rg -a -q 'Remote terminal closed \(exit status 0\)' "$qemu_log"
if rg -a -q 'accepting an unpinned host key' "$qemu_log"; then
  echo "QEMU test unexpectedly accepted an unpinned host key" >&2
  exit 1
fi
rg -q 'AUTH_PASSWORD accepted=True' "$server_log"
rg -q 'PTY_REQUEST .*accepted=True' "$server_log"
rg -q 'SHELL_REQUEST accepted=True' "$server_log"
rg -F -q "TERMINAL_INPUT data='\\x1b[12;1R'" "$server_log"
rg -q 'TERMINAL_DSR_OK row=12 column=1' "$server_log"
rg -q 'DHCP_ACK address=10.0.2.15' "$proxy_log"
rg -q 'ARP_REPLY gateway=10.0.2.2' "$proxy_log"
rg -q 'TCP_SYN guest_port=' "$proxy_log"

echo "QEMU_E2E_PASS arch=$arch build=$build_target"
echo "  application: $application"
echo "  qemu log:   $qemu_log"
echo "  ssh log:    $server_log"
echo "  proxy log:  $proxy_log"
