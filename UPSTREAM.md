# Upstream snapshot / 上游快照

- soloAS version / soloAS 版本：**0.1.2**。
- Current adaptation / 当前适配：**UE 5.8.4**。
- These are the original snapshots used for this adaptation. / 以下为本次适配所基于的原版快照。

## Epic Unreal Engine / Epic 虚幻引擎

| Field / 字段 | Value / 值 |
| --- | --- |
| Tag / 标签 | [epic.tag](.github/upstream.json) |
| Commit / 提交 | `f45729c0b6f097038e72b52d366a62baeca3275b` |
| Build version / 构建版本 | [epic.buildVersion / epic.installedBuild](.github/upstream.json) |

- The UE source repository is private; access requires an Epic account linked to GitHub. / UE 源码仓库为私有仓库，需要关联 Epic 账号与 GitHub 才能访问。

## Hazelight's UE fork (the Haze fork) / Hazelight 的 UE 分支（Haze fork）

- Documentation / 使用文档：[AngelScript documentation](https://angelscript.hazelight.se/).
- Repository / 仓库：https://github.com/Hazelight/UnrealEngine-Angelscript.git
- Branch / 分支：`angelscript-master`.
- Commit / 提交：`86158a28896e1d8af95671516f54c2e22dfe27e4`.

| Component / 组件 | Source path / 来源路径 |
| --- | --- |
| AngelscriptRuntime | `Engine/Plugins/Angelscript` |
| AngelscriptGAS | `Engine/Plugins/AngelscriptGAS` |
| AngelscriptEnhancedInput | `Engine/Plugins/AngelscriptEnhancedInput` |

- GAS and EnhancedInput source pins remain independent; see the per-product entries in the machine-readable snapshot. / GAS 与 EnhancedInput 保留独立来源 pin，见机器可读快照中的各产品记录。

- SoloASBridge contains the stock engine adaptation code. / SoloASBridge 承载原版引擎适配代码。
- Machine-readable snapshot / 机器可读快照：[upstream.json](.github/upstream.json).
