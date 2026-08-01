[Defines]
  PLATFORM_NAME                  = WolfSshPkg
  PLATFORM_GUID                  = 9C7B182E-942E-4AFE-91D8-3E1E55ED2BAC
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010006
  SUPPORTED_ARCHITECTURES        = AARCH64|X64
  BUILD_TARGETS                  = DEBUG|RELEASE|NOOPT
  SKUID_IDENTIFIER               = DEFAULT

!include MdePkg/MdeLibs.dsc.inc

[LibraryClasses]
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  DebugLib|MdePkg/Library/UefiDebugLibConOut/UefiDebugLibConOut.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  NetLib|NetworkPkg/Library/DxeNetLib/DxeNetLib.inf
  PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  UefiCompatLib|WolfSshPkg/Library/UefiCompatLib/UefiCompatLib.inf
  WolfCryptLib|WolfSshPkg/Library/WolfCryptLib/WolfCryptLib.inf
  WolfSshLib|WolfSshPkg/Library/WolfSshLib/WolfSshLib.inf

[BuildOptions]
  GCC:*_*_X64_CC_FLAGS = -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0

[Components]
  WolfSshPkg/Application/WolfSsh/WolfSsh.inf
