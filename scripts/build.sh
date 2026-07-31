#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "$0")/.." && pwd)"
edk2_root="${EDK2_ROOT:-$project_root/../upstream/edk2}"
toolchain_root="${AARCH64_TOOLCHAIN_ROOT:-$project_root/../toolchains/installed/xpack-aarch64-none-elf-gcc-15.2.1-1.1}"
build_root="$project_root/.build/edk2-workspace"

if [[ ! -f "$edk2_root/MdePkg/MdePkg.dec" ]]; then
  echo "EDK2_ROOT does not point to an EDK II checkout" >&2
  exit 2
fi
if [[ ! -x "$toolchain_root/bin/aarch64-none-elf-gcc" ]]; then
  echo "AARCH64_TOOLCHAIN_ROOT does not contain aarch64-none-elf-gcc" >&2
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
export GCCNOLTO_AARCH64_PREFIX="$toolchain_root/bin/aarch64-none-elf-"

build -a AARCH64 -t GCCNOLTO -b "${BUILD_TARGET:-DEBUG}" \
  -p WolfSshPkg/WolfSshPkg.dsc \
  -m WolfSshPkg/Application/WolfSsh/WolfSsh.inf \
  -n "${BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"

output="$build_root/Build/WolfSshPkg/${BUILD_TARGET:-DEBUG}_GCCNOLTO/AARCH64/WolfSsh.efi"
mkdir -p "$project_root/.build/output"
cp "$output" "$project_root/.build/output/wolfssh.efi"
"$edk2_root/BaseTools/Source/C/bin/GenFw" -z -r \
  "$project_root/.build/output/wolfssh.efi"
echo "$project_root/.build/output/wolfssh.efi"
