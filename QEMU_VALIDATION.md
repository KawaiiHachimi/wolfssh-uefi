# QEMU validation record

验证日期：2026-08-01（Asia/Tokyo）

## 被测产物

| 架构 | 构建目标 | 文件 | 大小 | SHA-256 | 文件类型 |
|---|---|---|---:|---|---|
| AARCH64 | EDK II `RELEASE_GCCNOLTO` | `dist/wolfssh-aarch64.efi` | 126,976 bytes | `7f46d1d78582dbc1a7d9c1a46c6145cd57ef8f75abee47ac0ab72e1a69223e7f` | PE32+ EFI application, AArch64 |
| X64 | EDK II `RELEASE_GCCNOLTO` | `dist/wolfssh-x64.efi` | 123,968 bytes | `4864358c28a767ed26a5217a7dad1bbc83a586b8bb2a3de56f0a8725c11a650b` | PE32+ EFI application, x86-64 |

## 环境

两个构建共用以下锁定源码：

| 组件 | 版本/提交 |
|---|---|
| EDK II 构建源码 | `ca8de19382c668cf8770ee788478edcd8a22d0e7` |
| wolfSSH | `be5331aa78bb078922eef0fba947c20ea161d7da` + UEFI PTY patch |
| wolfSSL | `41b7a0209abbddc579d3d861f36c0f574ae7e907` |
| UEFI Shell | v2.2 26H1，基于 `edk2-stable202602` |
| SSH 测试服务 | AsyncSSH 2.21.1 |

各架构的构建和虚拟机环境：

| 组件 | AARCH64 | X64 |
|---|---|---|
| 编译器 | xPack GNU AArch64 Embedded GCC 15.2.1-1.1 | Ubuntu GCC 13.3.0 + GNU binutils |
| 汇编器 | GNU assembler | NASM 2.16.01 |
| QEMU | xPack QEMU Arm 9.2.4 | QEMU 8.2.2（Ubuntu `1:8.2.2+ds-0ubuntu1.17`） |
| UEFI 固件 | xPack 所带 `edk2-stable202408-prebuilt.qemu.org` | OVMF `2024.02-2ubuntu0.9` |
| QEMU 机器 | `virt`, `cortex-a57` | `q35`, `max` CPU |

## 执行方式

```bash
ARCH=AARCH64 ./scripts/test-qemu.sh
ARCH=X64 ./scripts/test-qemu.sh
```

两次测试都使用 virtio-net、virtio-rng、UEFI pflash 和对应架构的 UEFI Shell，并在
每次运行时生成一次性 ECDSA P-256 SSH 主机密钥。测试环境不依赖特权网络配置；
`tests/qemu_net_proxy.py` 通过 QEMU stream netdev 提供隔离的测试 LAN：

- 客户端地址：`10.0.2.15/24`（DHCP）
- 测试网关/SSH 端：`10.0.2.2`
- SSH 代理到宿主 `127.0.0.1:2222`

## 已通过检查

| 层 | AARCH64 与 X64 的共同证据 |
|---|---|
| IPv4 配置 | `DHCP_OFFER`、`DHCP_ACK address=10.0.2.15` |
| 二层/三层到达 | `ARP_REPLY gateway=10.0.2.2` |
| TCP | `TCP_PROXY_CONNECTED`、`TCP_SYN` |
| 主机密钥固定 | 客户端以 `-f` 固定本次临时密钥，显示完全相同的 SHA-256 摘要，未走 `-y` |
| SSH 用户认证 | `AUTH_PASSWORD accepted=True` |
| PTY | `term=xterm-256color columns=80 rows=50 accepted=True` |
| Shell channel | `SHELL_REQUEST accepted=True` |
| ANSI 颜色 | `UEFI_WOLFSSH_COLOR_OK` |
| 擦除/回车 | `UEFI_WOLFSSH_ERASE_OK`；测试先定位到第 20 列，再通过 CR 回到第 1 列 |
| 光标定位 | `UEFI_WOLFSSH_CURSOR_OK` |
| 双向终端回应 | 服务端发送 `ESC[6n`，收到 `ESC[12;1R`，记录 `TERMINAL_DSR_OK` |
| 会话结束 | `UEFI_WOLFSSH_E2E_OK`，`Remote terminal closed (exit status 0)` |

本次原始日志中的临时主机密钥摘要分别为：

| 架构 | SHA-256 |
|---|---|
| AARCH64 | `57ea9ee73ab630f99e22ebf9854859ee07178413606057e82646dfed1f246478` |
| X64 | `e7434f4f6b9b8131d34089920f0e67cd0a8ea8083d6609ca4fe38e3db8fb64d7` |

原始日志保存在发布包的 `validation/`：

| 架构 | QEMU 控制台 | SSH 服务端 | 网络代理 |
|---|---|---|---|
| AARCH64 | `qemu-e2e.log` | `ssh-server.log` | `net-proxy.log` |
| X64 | `x64-qemu-e2e.log` | `x64-ssh-server.log` | `x64-net-proxy.log` |

AARCH64 QEMU 固件在网络/RNG 驱动加载前会打印一条早期 `ArmTrngLib` 初始化警告；
被测应用随后成功定位并使用了 `EFI_RNG_PROTOCOL`。应用本身在该协议不可用时会直接
拒绝 SSH 会话。

此验证证明两个架构在上述确定性环境中的完整链路可用，不等于对所有实体固件、NIC
驱动、SSH 服务器算法组合或完整 xterm 应用的兼容性认证。
