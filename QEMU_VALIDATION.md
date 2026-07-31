# QEMU validation record

验证日期：2026-08-01（Asia/Tokyo）

## 被测产物

| 项目 | 值 |
|---|---|
| 构建目标 | EDK II `RELEASE_GCCNOLTO`, AARCH64 |
| 文件 | `dist/wolfssh-aarch64.efi` |
| 大小 | 126,976 bytes |
| SHA-256 | `b6850661e58c878987f6b22b7e311cf5bb3730b13443b00e528aae628edeff88` |
| 文件类型 | PE32+ EFI application, AArch64 |

## 环境

| 组件 | 版本/提交 |
|---|---|
| QEMU | xPack QEMU Arm 9.2.4 |
| QEMU AARCH64 固件 | xPack 所带 `edk2-stable202408-prebuilt.qemu.org` |
| UEFI Shell | v2.2 26H1，基于 `edk2-stable202602` |
| EDK II 构建源码 | `ca8de19382c668cf8770ee788478edcd8a22d0e7` |
| wolfSSH | `be5331aa78bb078922eef0fba947c20ea161d7da` + UEFI PTY patch |
| wolfSSL | `41b7a0209abbddc579d3d861f36c0f574ae7e907` |
| 编译器 | xPack GNU AArch64 Embedded GCC 15.2.1-1.1 |
| SSH 测试服务 | AsyncSSH 2.21.1 |

## 执行方式

```bash
./scripts/test-qemu.sh
```

测试使用 QEMU `virt`、`cortex-a57`、virtio-net、virtio-rng 和 UEFI pflash，并在
每次运行时生成一次性 ECDSA P-256 SSH 主机密钥。由于这套 QEMU 构建不带 SLIRP，
`tests/qemu_net_proxy.py` 通过 QEMU stream netdev 提供一个隔离的测试 LAN：

- 客户端地址：`10.0.2.15/24`（DHCP）
- 测试网关/SSH 端：`10.0.2.2`
- SSH 代理到宿主 `127.0.0.1:2222`

## 已通过检查

| 层 | 证据 |
|---|---|
| IPv4 配置 | `DHCP_OFFER`、`DHCP_ACK address=10.0.2.15` |
| 二层/三层到达 | `ARP_REPLY gateway=10.0.2.2` |
| TCP | `TCP_PROXY_CONNECTED`、`TCP_SYN` |
| 主机密钥固定 | 本次客户端以 `-f 1d60...49ab` 固定当次临时密钥，显示完全相同的 SHA-256 摘要，未走 `-y` |
| SSH 用户认证 | `AUTH_PASSWORD accepted=True` |
| PTY | `term=xterm-256color columns=80 rows=50 accepted=True` |
| Shell channel | `SHELL_REQUEST accepted=True` |
| ANSI 颜色 | `UEFI_WOLFSSH_COLOR_OK` |
| 擦除/回车 | `UEFI_WOLFSSH_ERASE_OK`；测试先定位到第 20 列，再通过 CR 回到第 1 列 |
| 光标定位 | `UEFI_WOLFSSH_CURSOR_OK` |
| 双向终端回应 | 服务端发送 `ESC[6n`，收到 `ESC[12;1R`，记录 `TERMINAL_DSR_OK` |
| 会话结束 | `UEFI_WOLFSSH_E2E_OK`，`Remote terminal closed (exit status 0)` |

原始日志保存在发布包的 `validation/`：

- `qemu-e2e.log`
- `ssh-server.log`
- `net-proxy.log`

QEMU 固件在网络/RNG 驱动加载前会打印一条早期 `ArmTrngLib` 初始化警告；被测应用随后
成功定位并使用了 `EFI_RNG_PROTOCOL`。应用本身在该协议不可用时会直接拒绝 SSH 会话。

此验证证明上述确定性环境中的实现链路可用，不等于对所有实体固件、NIC 驱动、SSH
服务器算法组合或完整 xterm 应用的兼容性认证。
