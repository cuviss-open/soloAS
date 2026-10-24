[English](README.md) | 简体中文

# soloAS

## 文档

- [首页](https://soloas.cuviss.com/zh/)
- [安装 / 快速开始](https://soloas.cuviss.com/docs/zh/requirements.html)
- [脚本入门](https://soloas.cuviss.com/docs/zh/learn/index.html)
- [GAS](https://soloas.cuviss.com/docs/zh/manual-binding-packs.html#gas用脚本写技能和属性)
- [Enhanced Input](https://soloas.cuviss.com/docs/zh/manual-binding-packs.html#enhanced-input在脚本里响应输入)
- [API 参考](https://soloas.cuviss.com/docs/zh/learn/binding-manual-reference.html)
- [常见问题 / 差异](https://soloas.cuviss.com/docs/zh/core.html)
- [路线图](https://soloas.cuviss.com/zh/#roadmap)
- [UPSTREAM.md](UPSTREAM.md) · [CONTRIBUTING.md](CONTRIBUTING.md) · [LICENSE](LICENSE) · [RELEASE_NOTES.md](RELEASE_NOTES.md)

## 简介

- 以项目插件为未经修改的 Unreal Engine 提供 AngelScript 脚本工作流，兼容 Hazelight 的 UE 分支（Haze fork）的常见用法。
- 版本 **0.1.0+ue5.8.3** · 标签 **v0.1.0** · [发行下载](https://github.com/cuviss-open/soloAS/releases/tag/v0.1.0)。

## 上游快照

- 本次适配基于的 Epic UE 与 Hazelight 的 UE 分支（Haze fork）原版快照、commit 及组件来源路径：[UPSTREAM.md](UPSTREAM.md)、[upstream.json](upstream.json)。
- Hazelight 用法文档：[angelscript.hazelight.se](https://angelscript.hazelight.se/)。

## 包含的插件

| 源码包中的目录 | 插件身份 | 用途 / 启用方式 |
| --- | --- | --- |
| `AngelscriptRuntime/` | `Angelscript` | 核心运行时与编辑器集成；默认启用 |
| `SoloASBridge/` | `SoloASBridge` | 原版引擎适配；由运行时依赖启用 |
| `AngelscriptManualBindPlugins/AngelscriptGAS/` | `AngelscriptGAS` | 可选 Gameplay Ability System 绑定；默认关闭 |
| `AngelscriptManualBindPlugins/AngelscriptEnhancedInput/` | `AngelscriptEnhancedInput` | 可选 Enhanced Input 绑定；默认关闭 |

## 环境要求

- **Unreal Engine 5.8.3**：推荐 Launcher 安装版。
- 源码构建引擎（同一适配 tag）理论上功能等价，但不作保证。
- **Windows 11 x64** 为主要平台；Windows 10 理论上可用，尚未验证。
- **macOS 15+** 仅有有限编译验证，不保证行为。
- Win64 预编译包：蓝图工程在编辑器内可以直接使用。Windows 10 需要 7-Zip 解压；Windows 11 可以直接解压。
- Win64 工程打包（包括纯蓝图工程）需要 UE 支持的 C++ 工具链：MSVC 14.44 和 Windows SDK 10.0.22621.0；插件模块使用随包提供的预编译产物。
- 源码包：需要 C++ Unreal 项目及对应 Unreal C++ 工具链；Windows 使用 Visual Studio。纯蓝图项目先添加 C++ target。

## 快速开始

1. 从 [Releases](https://github.com/cuviss-open/soloAS/releases/tag/v0.1.0) 选择一个包，按 `SHA256SUMS` 核对：
   - **Win64 预编译包**：`soloAS-0.1.0-ue5.8.3-win64.7z`，包含源码及 Win64 二进制；解压到 `<Project>/Plugins/`，蓝图工程在编辑器内也能直接用。Windows 10 需要 7-Zip，Windows 11 可直接解压。
   - **源码包**：`soloAS-0.1.0-ue5.8.3-source.zip`，需要 C++ 工程和对应工具链。
   - 可选符号包：需要调试或查看崩溃堆栈时，下载 `soloAS-0.1.0-ue5.8.3-win64-symbols.7z`，解压覆盖到相同安装位置；该包仅包含 PDB 文件。
2. 关闭编辑器。使用源码包时，解压到 `<Project>/Plugins/soloAS/`，保留目录结构。保留 `AngelscriptRuntime/` 目录名：Unreal 通过 `Angelscript.uplugin` 识别插件。每个插件只安装一份。
3. 使用源码包时，从 `.uproject` 生成 Visual Studio 项目文件，构建 `<ProjectName>Editor`，配置为 **Win64 / Development**。使用预编译包时，直接打开项目。
4. 核心运行时会自动启用 `SoloASBridge`。需要时才在 Plugins 窗口启用 `AngelscriptGAS` 或 `AngelscriptEnhancedInput`，并按提示重启。
5. 将以下内容保存为 `<Project>/Script/FirstScriptActor.as`：

   ```angelscript
   class AFirstScriptActor : AActor
   {
       bool bHelloWorldShown = false;

       UFUNCTION(BlueprintOverride)
       void BeginPlay()
       {
           Print("Hello from soloAS!");
           bHelloWorldShown = true;
       }
   }
   ```

6. 将脚本 Actor 放入关卡或由游戏逻辑生成，然后运行游戏。`BeginPlay` 显示 **Hello from soloAS!** 五秒，并在 Output Log 中记录。

- 普通 `.as` 修改通过脚本编译与重载生效，无需重启编辑器。
- 修改原生插件代码或安装、更换插件后，关闭编辑器、重新生成项目文件并重建 Editor target。
- [详细安装说明](https://soloas.cuviss.com/docs/zh/requirements.html)

## VS Code

- 安装 Hazelight 的 **Unreal Angelscript** 扩展，参见 [AngelScript 文档](https://angelscript.hazelight.se/)。
- 将项目的 `Script/` 目录作为工作区打开。
- 补全、诊断、跳转、断点、变量查看与继续执行通过运行时 DebugServer 提供。
- 启动编辑器时不加 `-NoMessaging` 或 `-NoASDebugServer`，这两个参数会关闭该连接。

## 从 Haze fork 迁移

- 使用未经修改的 UE 5.8.3，以本包插件替换 fork 的 AngelScript 插件，清除重复安装。
- 项目脚本保留在 `Script/` 中，重新构建 C++ Editor target。
- 资产提示保存引擎版本为空时，使用 **SoloAS → Resave assets with empty engine version**，随后检查源控差异。
- [迁移指南](https://soloas.cuviss.com/docs/zh/migration.html)

## 脚本交付

- **源码**：打包 `.as` 文件，运行时在启动时编译；Pak 不能用于源码保密。
- **字节码**：使用 Shipping Game 可执行文件生成匹配的预编译数据，保留同次生成的三个配套文件 `Binds.Cache`、`Binds.Cache.Headers` 和 `PrecompiledScript.Cache`，打包时不包含主项目 `.as` 源码。不能用 Editor 缓存替代 Game 缓存。
- **生成 C++**：将支持的脚本函数生成代码编入游戏 C++ 模块，并保留配套的三个缓存；脚本、插件、引擎或其他原生代码变化后重新生成。
- 字节码不是加密或防逆向保护。
- [脚本交付命令与包内容检查](https://soloas.cuviss.com/docs/zh/packaging.html)

## 差异与限制

- FullReload 耗时约为 Haze fork 的 **1.6–1.8 倍**，属于已知适配成本；普通脚本修改不需要重启编辑器。
- 兼容范围为核心工作流和 fork 的常见用法，不代表包含 fork 的所有工具或 API。
- Windows 为主要验收平台；macOS 验证限于编译。
- [与 Haze 的差异](https://soloas.cuviss.com/docs/zh/core.html)

## 路线图

- 拟在未来版本提供：核心扩展、自动绑定、FeatureMod 支持、打包工具、手工绑定核心及更多绑定包。
- 这些是计划方向，范围、顺序和发布时间可能调整。
- [路线图](https://soloas.cuviss.com/zh/#roadmap)

## 许可

| 组件 | 许可证 |
| --- | --- |
| SoloASBridge | MPL-2.0 |
| AngelscriptRuntime 插件源码，含 soloAS 修改 | MIT |
| AngelScript 库，含修改 | zlib |
| AngelscriptGAS，含 soloAS 修改 | Apache-2.0 或 MIT，可任选其一 |
| AngelscriptEnhancedInput，含 soloAS 修改 | MIT |

- 参见 [NOTICE](NOTICE)、各插件的许可文件及保留的源码声明。
- 分发包含 MPL 代码的编译产品时，按 MPL-2.0 要求向接收者提供相应源码获取方式。

## 致谢与案例展示

- 以下两项均为**非强制约定**，不是额外许可条件。
- 希望在致谢、商店页、README 或网站中注明 **soloAS**，并链接到 <https://soloas.cuviss.com/>。
- 公开声明使用 soloAS 的项目，可被列为展示案例；仅使用项目名、简短介绍、公开截图或链接。
- 可应请求移除展示案例，欢迎主动投稿。

## 联系

- 问题与反馈：[GitHub Issues](https://github.com/cuviss-open/soloAS/issues)。
- 邮箱：[contact@cuviss.com](mailto:contact@cuviss.com)。
