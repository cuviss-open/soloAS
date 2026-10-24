# Upstream snapshot / 上游快照

- soloAS version / soloAS 版本：**0.1.1+ue5.8.3**。
- These are the original snapshots used for this adaptation. / 以下为本次适配所基于的原版快照。

## Epic Unreal Engine / Epic 虚幻引擎

| Field / 字段 | Value / 值 |
| --- | --- |
| Tag / 标签 | `5.8.3-release` |
| Commit / 提交 | `396c9f059903aed5fec78ecd3d437a40c6415368` |
| Build version / 构建版本 | `5.8.3-58210709+++UE5+Release-5.8` |

- The UE source repository is private; access requires an Epic account linked to GitHub. / UE 源码仓库为私有仓库，需要关联 Epic 账号与 GitHub 才能访问。

## Hazelight's UE fork (the Haze fork) / Hazelight 的 UE 分支（Haze fork）

- Documentation / 使用文档：[AngelScript documentation](https://angelscript.hazelight.se/).
- Repository / 仓库：https://github.com/Hazelight/UnrealEngine-Angelscript.git
- Branch / 分支：`angelscript-master`.
- Commit / 提交：`4b2892f0b550f20f4856d38f68d4987e3dfdb7da`.

| Component / 组件 | Source path / 来源路径 |
| --- | --- |
| AngelscriptRuntime | `Engine/Plugins/Angelscript` |
| AngelscriptGAS | `Engine/Plugins/AngelscriptGAS` |
| AngelscriptEnhancedInput | `Engine/Plugins/AngelscriptEnhancedInput` |

- SoloASBridge contains the stock engine adaptation code. / SoloASBridge 承载原版引擎适配代码。
- Machine-readable snapshot / 机器可读快照：[upstream.json](.github/upstream.json).
