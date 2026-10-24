[English](README.md) | 简体中文

# soloAS

soloAS 是一个为设计而生的 Unreal 脚本系统。它以原版引擎插件的形式提供 AngelScript 脚本支持，无需修改引擎代码，却也能拥有优秀的开发体验。

## 文档

[项目介绍](https://soloas.cuviss.com/docs/zh/overview.html)

## 下载

[Release v0.1.1](https://github.com/cuviss-open/soloAS/releases)：Win64 预编译包（源码与二进制）、符号包（PDB 调试符号）、源码包（需要自行编译）。

## 包含的插件

| 源码包中的目录 | 插件身份 | 用途 / 启用方式 |
| --- | --- | --- |
| `AngelscriptRuntime/` | `Angelscript` | 核心运行时与编辑器集成；默认启用 |
| `SoloASBridge/` | `SoloASBridge` | 原版引擎适配；由运行时依赖启用 |
| `AngelscriptManualBindPlugins/AngelscriptGAS/` | `AngelscriptGAS` | 可选 Gameplay Ability System 绑定；默认关闭 |
| `AngelscriptManualBindPlugins/AngelscriptEnhancedInput/` | `AngelscriptEnhancedInput` | 可选 Enhanced Input 绑定；默认关闭 |

## 许可

| 组件 | 许可证 |
| --- | --- |
| AngelscriptRuntime（包含 AngelScript 库） | MIT，其中 AngelScript 库为 zlib |
| AngelscriptGAS | Apache-2.0 或 MIT，任选其一 |
| AngelscriptEnhancedInput | MIT |
| 其余所有内容（包括 SoloASBridge） | MPL-2.0 |

- 参见 [NOTICE](NOTICE)、各插件的许可文件及保留的源码声明。

注：凡是从 UE Haze fork 继承来的插件，我们对其所做的修改沿用该插件原有的许可证；除此之外，皆为 MPL-2.0。

致谢与案例展示（自愿）：欢迎注明使用了 soloAS 并分享公开项目；案例可应请求修改或撤下。

联系：[GitHub Issues](https://github.com/cuviss-open/soloAS/issues) · [contact@cuviss.com](mailto:contact@cuviss.com)。

[UPSTREAM.md](UPSTREAM.md) · [CONTRIBUTING.md](CONTRIBUTING.md) · [LICENSE](LICENSE)
