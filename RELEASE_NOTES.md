# soloAS 0.1.0 (UE 5.8.3)

## English

- Version: `0.1.0+ue5.8.3`; tag: `v0.1.0`.
- Win64 package: `soloAS-0.1.0-ue5.8.3-win64.7z`, source plus precompiled Win64 binaries; extract into project `Plugins/`, including for Blueprint projects in the editor. Windows 10 needs 7-Zip; Windows 11 can extract it directly.
- Symbols package: `soloAS-0.1.0-ue5.8.3-win64-symbols.7z`, PDB files only; optionally extract over the same installation for debugging or crash stack analysis.
- Packaging Win64 projects, including Blueprint-only projects, requires MSVC 14.44 and Windows SDK 10.0.22621.0; supplied plugin modules remain precompiled.
- Source package: `soloAS-0.1.0-ue5.8.3-source.zip`; requires a C++ project and matching toolchain. Verify all packages with `SHA256SUMS`.
- Includes AngelscriptRuntime, SoloASBridge, AngelscriptGAS and AngelscriptEnhancedInput.
- The core runtime is enabled by default and enables Bridge; GAS and Enhanced Input bindings are optional and off by default.
- Provides native-class script inheritance, reflected properties and functions, Blueprint event overrides, and calls to bound Unreal APIs.
- Ordinary script edits compile and reload without an editor restart.
- Script delivery supports source, matching Game bytecode caches, and generated C++ compiled into the game module with the matching caches.
- GAS supports manual GameplayTask activation through `ReadyForActivation` after configuring task properties.
- FullReload takes about 1.6–1.8× the Haze fork's time, a known adaptation cost.
- Recommends Launcher UE 5.8.3. Same-tag source builds are theoretically equivalent but not guaranteed; modified engines are unsupported.
- Windows is primary; macOS has limited compile verification.
- Script interfaces, capturing lambdas and Blueprint parent import are outside this release; future extensions are planned.
- Includes English and Chinese setup instructions, script delivery links and component license information; see README and NOTICE.

## 中文

- 版本：`0.1.0+ue5.8.3`；标签：`v0.1.0`。
- Win64 包：`soloAS-0.1.0-ue5.8.3-win64.7z`，源码加预编译 Win64 二进制；解压到项目 `Plugins/`，蓝图工程在编辑器内也能直接用。Windows 10 需要 7-Zip，Windows 11 可直接解压。
- 符号包：`soloAS-0.1.0-ue5.8.3-win64-symbols.7z`，仅含 PDB；需要调试或查看崩溃堆栈时，解压覆盖到相同安装位置。
- Win64 工程打包（包括纯蓝图工程）需要 UE 支持的 C++ 工具链：MSVC 14.44 和 Windows SDK 10.0.22621.0；插件模块使用随包提供的预编译产物。
- 源码包：`soloAS-0.1.0-ue5.8.3-source.zip`，需要 C++ 工程及对应工具链；所有包均使用 `SHA256SUMS` 核验。
- 包含 AngelscriptRuntime、SoloASBridge、AngelscriptGAS 与 AngelscriptEnhancedInput。
- 核心运行时默认启用并启用 Bridge；GAS 和 Enhanced Input 绑定可选、默认关闭。
- 支持脚本继承原生类、反射属性和函数、覆盖 Blueprint 事件及调用已绑定的 Unreal API。
- 普通脚本修改通过编译与重载生效，无需重启编辑器。
- 脚本交付支持源码、配套 Game 字节码缓存，以及编入游戏模块并配套缓存的生成 C++。
- GAS 支持配置任务属性后通过 `ReadyForActivation` 手动激活 GameplayTask。
- FullReload 耗时约为 Haze fork 的 1.6–1.8 倍，属于已知适配成本。
- 推荐 Launcher UE 5.8.3；相同 tag 源码构建理论等价但不保证；不支持修改过的引擎。
- Windows 为主要平台；macOS 仅有有限编译验证。
- 脚本接口、捕获 lambda 和 Blueprint 父类导入不在本版范围内；未来扩展为计划方向。
- 附中英文安装说明、脚本交付链接及组件许可信息，参见 README 和 NOTICE。

## Upstream snapshot / 上游快照

- Epic UE: tag `5.8.3-release`, commit `396c9f059903aed5fec78ecd3d437a40c6415368`, build version `5.8.3-58210709+++UE5+Release-5.8`.
- Hazelight fork: branch `angelscript-master`, commit `4b2892f0b550f20f4856d38f68d4987e3dfdb7da`; see the [documentation](https://angelscript.hazelight.se/).
- 本次适配所基于的 Epic UE 与 Hazelight 原版快照如上；各组件来源路径与仓库地址见 [UPSTREAM.md](UPSTREAM.md)，机器可读记录见 [upstream.json](upstream.json)。
- UE source access requires an Epic account linked to GitHub. / UE 私有源码仓库需要关联 Epic 账号与 GitHub 才能访问。
