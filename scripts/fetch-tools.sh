#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "$0")/.." && pwd)"
tool_root="${TOOL_ROOT:-$project_root/../toolchains}"
download_root="$tool_root/downloads"
installed_root="$tool_root/installed"

# shellcheck disable=SC1091
source "$project_root/deps.lock"

download_checked() {
  local url="$1"
  local sha256="$2"
  local destination="$3"
  local temporary

  if [[ -f "$destination" ]]; then
    if printf '%s  %s\n' "$sha256" "$destination" | sha256sum --check --status; then
      echo "Already downloaded: $destination"
      return
    fi
    echo "Checksum mismatch for existing file: $destination" >&2
    return 1
  fi

  temporary="$(mktemp "$download_root/download.XXXXXX")"
  if ! curl --fail --location --retry 3 --output "$temporary" "$url"; then
    rm -f "$temporary"
    return 1
  fi
  if ! printf '%s  %s\n' "$sha256" "$temporary" | sha256sum --check --status; then
    rm -f "$temporary"
    echo "Checksum verification failed for $url" >&2
    return 1
  fi
  mv "$temporary" "$destination"
}

mkdir -p "$download_root" "$installed_root"

gcc_archive="$download_root/$(basename "$AARCH64_TOOLCHAIN_URL")"
qemu_archive="$download_root/$(basename "$QEMU_AARCH64_URL")"
shell_binary="$download_root/shellaa64-26H1.efi"
download_checked "$AARCH64_TOOLCHAIN_URL" "$AARCH64_TOOLCHAIN_SHA256" \
  "$gcc_archive"
download_checked "$QEMU_AARCH64_URL" "$QEMU_AARCH64_SHA256" \
  "$qemu_archive"
download_checked "$SHELLAA64_URL" "$SHELLAA64_SHA256" "$shell_binary"

gcc_directory="$installed_root/xpack-aarch64-none-elf-gcc-15.2.1-1.1"
qemu_directory="$installed_root/xpack-qemu-arm-9.2.4-1"
if [[ ! -x "$gcc_directory/bin/aarch64-none-elf-gcc" ]]; then
  tar -xzf "$gcc_archive" -C "$installed_root"
fi
if [[ ! -x "$qemu_directory/bin/qemu-system-aarch64" ]]; then
  tar -xzf "$qemu_archive" -C "$installed_root"
fi

python_root="$tool_root/python"
if ! PYTHONPATH="$python_root" python3 -c \
    "import asyncssh; assert asyncssh.__version__ == '$ASYNCSSH_VERSION'" \
    2>/dev/null; then
  python3 -m pip install --disable-pip-version-check --target "$python_root" \
    "asyncssh==$ASYNCSSH_VERSION"
fi

echo "Pinned compiler, QEMU, UEFI Shell and QEMU test dependencies are ready"
