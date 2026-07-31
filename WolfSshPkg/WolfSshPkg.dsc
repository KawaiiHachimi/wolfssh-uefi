[Defines]
  PLATFORM_NAME                  = WolfSshPkg
  PLATFORM_GUID                  = 9C7B182E-942E-4AFE-91D8-3E1E55ED2BAC
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010006
  SUPPORTED_ARCHITECTURES        = AARCH64
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

[LibraryClasses.AARCH64]
  ArmGenericTimerCounterLib|ArmPkg/Library/ArmGenericTimerPhyCounterLib/ArmGenericTimerPhyCounterLib.inf
  ArmLib|MdePkg/Library/ArmLib/ArmBaseLib.inf
  TimerLib|ArmPkg/Library/ArmArchTimerLib/ArmArchTimerLib.inf

[Components]
  WolfSshPkg/Application/WolfSsh/WolfSsh.inf
