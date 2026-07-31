# Third-party notices

本项目固定并使用以下第三方组件。准确版本见 `deps.lock`。

| 组件 | 用途 | 版本 | 许可证说明 |
|---|---|---|---|
| [wolfSSH](https://github.com/wolfSSL/wolfssh) | SSHv2 客户端实现 | `be5331aa78bb078922eef0fba947c20ea161d7da`，另加 `patches/wolfssh-uefi-terminal.patch` | GPLv3 或 wolfSSL Inc. 商业许可；见上游 `LICENSING` |
| [wolfSSL/wolfCrypt](https://github.com/wolfSSL/wolfssl) | ECC、AES、GCM、HMAC、SHA-256 | `41b7a0209abbddc579d3d861f36c0f574ae7e907` | GPLv3（上游列出的例外除外）或 wolfSSL Inc. 商业许可；见上游 `COPYING`、`LICENSING` |
| [EDK II](https://github.com/tianocore/edk2) | UEFI 头文件、BaseTools、库和网络协议 | `ca8de19382c668cf8770ee788478edcd8a22d0e7` | BSD-2-Clause-Patent；见上游 `License.txt`。其子模块保留各自许可证 |
| [iperf3-uefi](https://github.com/BigfootACA/iperf3-uefi) | TCP4 生命周期和默认 IPv4 映射的设计参考，不链接进二进制 | `45efb962dd390a7f32ce28bb65b43cff2e707d72` | BSD-2-Clause；见上游 `LICENSE` |
| [AsyncSSH](https://github.com/ronf/asyncssh) | 仅用于 QEMU 端到端测试 | 2.21.1 | EPL-2.0 OR GPL-2.0-or-later |

QEMU、xPack GNU AArch64 Embedded GCC 和 pbatard UEFI-Shell 仅作为构建/测试工具下载，
不包含在源码发布包内。它们分别受各自上游许可证约束。

本目录中的 `LICENSE` 是 GNU GPL version 3 的完整文本。使用 wolfSSH/wolfSSL 商业
许可证构建或再分发时，不应假定该商业许可自动覆盖本项目或其他第三方组件；请自行
完成许可证审查。
