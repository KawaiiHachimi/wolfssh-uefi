#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "$0")/.." && pwd)"
deps_root="${DEPS_ROOT:-$project_root/../upstream}"

# shellcheck disable=SC1091
source "$project_root/deps.lock"

ensure_checkout() {
  local repository="$1"
  local commit="$2"
  local destination="$3"
  local name="$4"

  if [[ -d "$destination/.git" ]]; then
    if [[ "$(git -C "$destination" rev-parse HEAD)" != "$commit" ]]; then
      echo "$name exists at a different commit: $destination" >&2
      return 1
    fi
    echo "$name already pinned at $commit"
    return
  fi
  if [[ -e "$destination" ]]; then
    echo "$name destination exists but is not a Git checkout: $destination" >&2
    return 1
  fi

  mkdir -p "$(dirname "$destination")"
  git init -q "$destination"
  git -C "$destination" remote add origin "$repository"
  git -C "$destination" fetch --depth 1 origin "$commit"
  git -C "$destination" checkout -q --detach FETCH_HEAD
  if [[ "$(git -C "$destination" rev-parse HEAD)" != "$commit" ]]; then
    echo "$name checkout verification failed" >&2
    return 1
  fi
}

ensure_link() {
  local target="$1"
  local link="$2"

  if [[ -e "$link" && ! -L "$link" ]]; then
    echo "Refusing to replace non-symlink path: $link" >&2
    return 1
  fi
  ln -sfn "$target" "$link"
}

ensure_path_link() {
  local target="$1"
  local link="$2"
  local relative_target

  relative_target="$(realpath --relative-to="$(dirname "$link")" "$target")"
  ensure_link "$relative_target" "$link"
}

ensure_checkout "$EDK2_REPOSITORY" "$EDK2_COMMIT" \
  "$deps_root/edk2" "EDK II"
git -C "$deps_root/edk2" submodule update --init --depth 1 -- \
  MdeModulePkg/Library/BrotliCustomDecompressLib/brotli \
  MdePkg/Library/MipiSysTLib/mipisyst
if [[ "$(git -C "$deps_root/edk2/MdeModulePkg/Library/BrotliCustomDecompressLib/brotli" rev-parse HEAD)" != "$EDK2_BROTLI_COMMIT" ||
      "$(git -C "$deps_root/edk2/MdePkg/Library/MipiSysTLib/mipisyst" rev-parse HEAD)" != "$EDK2_MIPISYST_COMMIT" ]]; then
  echo "EDK II submodule verification failed" >&2
  exit 1
fi

ensure_checkout "$WOLFSSL_REPOSITORY" "$WOLFSSL_COMMIT" \
  "$deps_root/wolfssl" "wolfSSL"
ensure_checkout "$WOLFSSH_REPOSITORY" "$WOLFSSH_COMMIT" \
  "$deps_root/wolfssh" "wolfSSH"

if git -C "$deps_root/wolfssh" apply --check \
    "$project_root/patches/wolfssh-uefi-terminal.patch" 2>/dev/null; then
  git -C "$deps_root/wolfssh" apply \
    "$project_root/patches/wolfssh-uefi-terminal.patch"
  echo "Applied wolfSSH UEFI terminal patch"
elif git -C "$deps_root/wolfssh" apply --reverse --check \
    "$project_root/patches/wolfssh-uefi-terminal.patch" 2>/dev/null; then
  echo "wolfSSH UEFI terminal patch is already applied"
else
  echo "wolfSSH UEFI terminal patch does not apply cleanly" >&2
  exit 1
fi

if [[ "${FETCH_REFERENCE_PROJECTS:-0}" == "1" ]]; then
  ensure_checkout "$IPERF3_UEFI_REPOSITORY" "$IPERF3_UEFI_COMMIT" \
    "$deps_root/iperf3-uefi" "iperf3-uefi"
fi

mkdir -p "$project_root/WolfSshPkg/ThirdParty"
ensure_path_link "$deps_root/wolfssl" \
  "$project_root/WolfSshPkg/ThirdParty/wolfssl"
ensure_path_link "$deps_root/wolfssh" \
  "$project_root/WolfSshPkg/ThirdParty/wolfssh"
ensure_link ../ThirdParty/wolfssl/wolfssl \
  "$project_root/WolfSshPkg/Include/wolfssl"
ensure_link ../ThirdParty/wolfssl/wolfcrypt \
  "$project_root/WolfSshPkg/Include/wolfcrypt"
ensure_link ../ThirdParty/wolfssh/wolfssh \
  "$project_root/WolfSshPkg/Include/wolfssh"
ensure_link ../ThirdParty/wolfssh/src \
  "$project_root/WolfSshPkg/Include/src"

echo "Pinned source dependencies are ready under $deps_root"
