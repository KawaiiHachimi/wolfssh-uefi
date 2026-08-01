#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "$0")/.." && pwd)"
edk2_root="${EDK2_ROOT:-$project_root/../upstream/edk2}"
build_root="$project_root/.build/edk2-workspace"
arch="${ARCH:-AARCH64}"
toolchain_tag="GCCNOLTO"

case "$arch" in
  AARCH64)
    artifact_arch="aarch64"
    toolchain_root="${AARCH64_TOOLCHAIN_ROOT:-$project_root/../toolchains/installed/xpack-aarch64-none-elf-gcc-15.2.1-1.1}"
    if [[ ! -x "$toolchain_root/bin/aarch64-none-elf-gcc" ]]; then
      echo "AARCH64_TOOLCHAIN_ROOT does not contain aarch64-none-elf-gcc" >&2
      exit 2
    fi
    export GCCNOLTO_AARCH64_PREFIX="$toolchain_root/bin/aarch64-none-elf-"
    ;;
  X64)
    artifact_arch="x64"
    for required_tool in gcc ar objcopy nasm; do
      if ! command -v "$required_tool" >/dev/null 2>&1; then
        echo "X64 build requires $required_tool in PATH" >&2
        exit 2
      fi
    done
    ;;
  *)
    echo "Unsupported ARCH=$arch; expected AARCH64 or X64" >&2
    exit 2
    ;;
esac

if [[ ! -f "$edk2_root/MdePkg/MdePkg.dec" ]]; then
  echo "EDK2_ROOT does not point to an EDK II checkout" >&2
  exit 2
fi
if [[ ! -f "$project_root/WolfSshPkg/ThirdParty/wolfssh/src/ssh.c" ||
      ! -f "$project_root/WolfSshPkg/ThirdParty/wolfssl/wolfcrypt/src/aes.c" ]]; then
  echo "wolfSSH/wolfSSL sources are missing; run scripts/fetch-deps.sh" >&2
  exit 2
fi
if [[ ! -x "$edk2_root/BaseTools/Source/C/bin/GenFw" ]]; then
  make -C "$edk2_root/BaseTools/Source/C" GenFw \
    CROSS_LIB_UUID=unused PYTHON_COMMAND=python3
fi

mkdir -p "$build_root"
mkdir -p "$build_root/Conf"
for config in target tools_def build_rule; do
  if [[ ! -f "$build_root/Conf/${config}.txt" ]]; then
    cp "$edk2_root/BaseTools/Conf/${config}.template" \
      "$build_root/Conf/${config}.txt"
  fi
done
export WORKSPACE="$build_root"
export PACKAGES_PATH="$edk2_root:$project_root"
export EDK_TOOLS_PATH="$edk2_root/BaseTools"
export PYTHONPATH="$EDK_TOOLS_PATH/Source/Python${PYTHONPATH:+:$PYTHONPATH}"
export PATH="$EDK_TOOLS_PATH/BinWrappers/PosixLike:$EDK_TOOLS_PATH/Source/C/bin:$PATH"

build -a "$arch" -t "$toolchain_tag" -b "${BUILD_TARGET:-DEBUG}" \
  -p WolfSshPkg/WolfSshPkg.dsc \
  -m WolfSshPkg/Application/WolfSsh/WolfSsh.inf \
  -n "${BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"

output="$build_root/Build/WolfSshPkg/${BUILD_TARGET:-DEBUG}_${toolchain_tag}/$arch/WolfSsh.efi"
mkdir -p "$project_root/.build/output"
artifact="$project_root/.build/output/wolfssh-$artifact_arch.efi"
cp "$output" "$artifact"
"$edk2_root/BaseTools/Source/C/bin/GenFw" -z -r \
  "$artifact"
cp "$artifact" "$project_root/.build/output/wolfssh.efi"
echo "$artifact"
