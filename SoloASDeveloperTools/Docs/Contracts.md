# SoloAS 开发工具公共合同 v3

0.2 的打包执行、窗口、工具链和 MCP Host 共用本合同。插件名与安装目录为 `SoloASDeveloperTools`。本插件包含 Providers、Editor 和 Win64-only SoloASDeveloperToolsToolchainEditor 三个 Editor 模块；工具链模块在 Default 阶段加载，产品依赖仅为 Angelscript 与 SoloASBridge；不进入 Game/Shipping。Windows 为本轮验证目标，Mac 尚无编译或运行保证。配置开关只承诺重启 Editor/新建 commandlet 进程后生效。

## 文件与职责

- `Source/SoloASDeveloperToolsProviders/Public/SoloASDeveloperToolsProviders.h`：无 UI 的 provider 值类型、接口、注册/撤销和请求分发。
- `Source/SoloASDeveloperToolsEditor/Private/Commandlets/`：有界文件请求桥。
- `Source/SoloASDeveloperToolsEditor/Private/Providers/`：后续内建 provider 实现位置；本轮不提供 AutoBind、ModKit 或 CoreExtensions 正式 provider。
- `Source/SoloASDeveloperToolsEditor/Private/PackagingWindow/`：窗口与 CLI 后端；窗口注册固定 tab，公共菜单通过 OpenPackagingWindow 调用。
- `Build/SoloASMcpHost/`：独立 stdio MCP Host、CLI 路由与 provider 请求分发，启动和协议说明见 [Host README](../Build/SoloASMcpHost/README.md)。
- `Build/PackagingTool/config.py`：配置解析、三层合并、能力缓存集合、项目 ini 定点保存与方案保存。
- `Build/PackagingTool/soloas_package.py`：CLI、job 持久化与状态机；真实打包通过兄弟文件 packaging.py 委托给 C 线。

Python 需要 3.10+，无第三方依赖。窗口与 MCP 通过参数数组调用此入口，不能分别复制配置合并或状态机实现。

## 配置、方案与生效输入

方案文件：项目 `Config/SoloASPackagingProfiles.json`，可版本管理。schemaVersion 为整数 1；顶层只接受 schemaVersion 与 profiles。缺文件等同于没有命名方案，显式选择不存在的方案返回 profile_missing。

配置字段：platform 为 Win64/Mac/Linux；target 为 UE target 标识符，默认项目名；configuration 为 Debug/DebugGame/Development/Test/Shipping，默认 Shipping；deliveryMode 为 source/bytecode/semi_aot，默认 source；maps/cookDirectories/extraFiles/requiredProviders 为非空字符串数组，默认空；pak 默认 true、ioStore 默认 false；outputDirectory 为项目相对 POSIX 路径，不接受绝对路径、盘符、反斜杠或父级遍历，默认 Saved/Packages。未知字段和错误类型拒绝。平台可表示不等于该平台已通过打包验收。

生效顺序：内建默认值 → Config/DefaultGame.ini → Config/<platform>/<platform>Game.ini → 方案 overrides → 本次 --overrides → 显式 --platform。同层数组整体替换。平台 ini 按上述最高优先级的平台选择；未显式选择时默认 Win64。

已读 ini section：`[/Script/UnrealEd.ProjectPackagingSettings]`。已映射键：BuildTarget、BuildConfiguration（剥离 PPBC_）、UsePakFile、bUseIoStore；数组为 MapsToCook=(FilePath="...")、DirectoriesToAlwaysCook=(Path="...")、DirectoriesToAlwaysStageAsNonUFS=(Path="...")。支持 + 去重追加、. 保留重复追加、- 移除同值、! 清空、无前缀替换。空 BuildTarget 沿用项目名。未知 section/key 不改写。这里只读取项目显式覆盖与工具默认值，不声称已解析 UE 引擎全量 ini 层级或得到 UAT 最终生效配置。

`config read` 返回 project（文件名）、profile（名称或 null）、projectValues、profileOverrides、effective、sources（每字段的 default/project_name/ini 相对路径/profile/invocation）、configDigest。digest 为 effective 的 UTF-8 JSON（sort_keys=True，separators=(',', ':')，默认 ensure_ascii=True）的 SHA256，只标识配置，不证明原生构建、缓存或 JIT 配套。**C 线须另冻结引擎/插件/源码/生成物身份；不得拿 configDigest 冒充完整输入摘要。**

保存项目设置使用 config write，保存方案使用 profiles save；D/E 不自行写 ini 或方案文件。具体语法见下文。

### extraFiles 路径合同（C/D/E）

extraFiles 每个字符串就是 UE `DirectoriesToAlwaysStageAsNonUFS` / `DirectoriesToAlwaysStageAsUFS` 的 `Path` 语义：**相对项目 Content 目录**。当前 config 读写映射仍为 NonUFS；没有新增 UFS 类型选择或混合数组。ini 读取、方案和 CLI `--overrides`、config write 使用相同规则，保留路径原文，不加、不去 `Content/` 或其他前缀，也不转换分隔符。允许相对路径中的 `/` 或 `\`，校验时两者都视为分隔符。非空；绝对路径、盘符（包括 `C:Data`）、UNC、任何 `..` 路径段、NUL、引号和换行一律拒绝，返回 invalid_config/exit2；不会先写文件再报错。不要求配置时目录已经存在。

| 输入 path | 项目内含义 / 结果 |
| --- | --- |
| `Packaging/Readme.txt` | `<project>/Content/Packaging/Readme.txt`，原样写 `(Path="Packaging/Readme.txt")` |
| `Data/Tables` | `<project>/Content/Data/Tables`，CLI 与 ini 原样往返 |
| `Content/Nested` | `<project>/Content/Content/Nested`；这是字面相对路径，不自动去掉首段 |
| `../Outside`、`Data/../../Outside`、`Data\..\Outside` | 拒绝父级遍历 |
| `/Game/Data`、`/tmp/Data`、`C:/Data`、`C:\Data`、`\\server\share` | 拒绝绝对路径；`/Game` 资产路径不适用于 extraFiles |

包内落点与 UFS/NonUFS 暂存由 UE stage 规则决定；C 线把该配置交给 UE，不另行复制 extraFiles，也不按项目根拼接路径。此配置校验不代表 UE 最终暂存或容器验证已通过。


## CLI 与响应

入口示例（--project 与 --json 可在任意位置）：

config read / precheck / run / clean-cache / generate-precompiled / sync-jit 支持 --overrides '<JSON object>'。目录形式的 --project 必须恰有一个 .uproject。正常业务响应 stdout 恰好一个 JSON 对象，不混日志；--json 使用单行紧凑形式，否则缩进展示相同对象。--help 属于 argparse 文本帮助。

失败 error 为 `{ "code": "stable_machine_code", "message": "human readable" }`，data 仍可包含失败 job 或检查细节。退出码：0 查询成功/配置预检有效；2 输入或 I/O 错误；3 precheck/provider 前置条件失败；4 打包后端缺失、导入失败或接口缺失。`status` 成功读取失败 job 时退出 0，须检查 data.job.state 判断作业结果。

profiles list 的 data.profiles 是按名称排序的 `{name, description?, overrides?}` 数组。precheck 校验项目、配置与 requiredProviders，可用 --editor 显式接入只读工具链 commandlet；返回 scope=configuration、configurationValid、readyToPackage=false、checks、pendingChecks、errors、config、cacheRequirements、toolchain。工具链模块缺失时返回 unavailable(module-missing) 且保留 pending；模块已集成但未指定 Editor 时同样保留 pending；target 存在性、空间、provider 实机发现与原生输入仍 pending，不授予可打包资格。缺少 requiredProvider 的离线证明返回 provider_discovery_required，不能静默省略。

## Job 与执行接入（C 线）

`run` 为同步语义：完成或失败后返回 data.job。缺少后端的安装会落盘 queued → prechecking → failed（backend_not_implemented），退出 4。集成后端在独立发布副本中执行构建、生成、暂存和包检查，不向开发工程写入 JIT 模块或发布配置。D/E 以后台子进程调用同步 run，并通过 --job-id 指定预先生成的 ID 以便并行查询；没有隐藏的 daemon 或 detached 执行器。

目录：`Saved/SoloASDeveloperTools/Packaging/<job-id>/`。job-id 为随机 32 位小写十六进制；input.json 保存本轮解析配置，不覆盖；job.json 原子替换（同目录 tmp → replace）。单一 executor 拥有 job 写权，status 跨进程只读。

job.json 字段：schemaVersion=1、contractVersion=3（v1 作业可缺）、jobId、state、phase、createdAt/updatedAt（UTC ISO8601）、configDigest、profile、input="input.json"、log="job.log"（v1 可缺）、result（当前 null，C 线成功时定义产物）、error（失败代码/说明或 null）、history（按序的 state/phase/at）。状态边：

`transition(directory, job, state, phase=..., error=...)` 校验状态边/阶段并持久化；running → running 仅用于推进非空阶段，并追加 history。PHASES 固定为 precheck、prepare、generate、collect、jit_sync、build、stage、package_check；provider 插入点见下一节。C 线通过 packaging.run_job(project, resolved) 接入；阶段不能跳过必要 provider 或在失败后伪造 succeeded。真实取消须停止/等待整个自有子进程树与恢复临时修改后才落 cancelled。cancel 对终态幂等返回原 job；非终态只写 cancel.request.json，返回 cancelRequested=true，不伪造状态转换。executor 必须轮询并在清理结束后推进终态。

## 缓存能力集合（C 线）

config.cache_requirements(capabilities) 返回 required 与 requiredCapabilities。必需集合为 Binds.Cache、Binds.Cache.Headers、PrecompiledScript.Cache；core 与 core_extensions 不增加缓存文件。未知能力拒绝。

config.check_caches(directory, capabilities) 返回结构错误数组；未知额外产物不按数量拒绝。**这只是存在性、非空与集合检查，不证明同轮、GUID、构建身份、JIT 链接或容器内正确性。**模块缓存等后续能力以独立集合扩展，不写死“五个文件”。核心扩展 provider 的正式能力发现由后续接入负责，不凭磁盘残留猜已启用能力。

## Provider ABI 与生命周期（A/C/E 线）

公共 namespace：SoloASDeveloperTools，ContractVersion=3。两个 IModularFeatures 名称分别为 SoloASPackagingStepProvider 与 SoloASMcpToolProvider。仅使用公共 Register/Unregister 重载；不要绕过校验直接注册 feature。

GetProviderId 为稳定非空小写字母/数字/_/-，保留 soloas；同一 feature 内 ID 唯一，同一产品可用相同 ID 注册两种接口。GetVersion 非空并冻结于注册期。步骤/工具名以 `<providerId>.` 开头，不能覆盖核心命名空间或另一 provider。注册后 ID、版本、描述符不可变。重复注册返回 false/duplicate_provider，不覆盖既有实例。

Packaging FPackagingStep：Name、Phase、Modes、Inputs、Artifacts。Phase 只允许 precheck、after_generate、before_jit_sync、before_stage、after_package_check；Modes 为 source/bytecode/semi_aot 非空子集；Inputs/Artifacts 为 provider 定义的声明字符串数组，不自动当作可执行路径。Execute(Name, JobId, ArgumentsJson)、Cancel(JobId)、Quiesce() 由 provider 实现。

MCP FMcpTool：Name、Description、InputSchemaJson（JSON object，type=object）、bReadOnly、bDestructive；Call(Name, JobId, ArgumentsJson)、Cancel、Quiesce。E 线转为 MCP tools/list 的 inputSchema 与 annotations，保持副作用属性，不统一标成只读。ResultJson 必须是 JSON object；FCallResult 包含 bOk、Code、Message 与 ResultJson，成功坏 JSON 转 invalid_provider_result。

注册、撤销、发现、调用均在 game thread 同步执行。每次查询动态枚举并按 providerId 排序，不保存裸 provider 指针、委托或工具快照；因此 provider 先加载、工具先加载以及消费者关闭/重开使用同一路径。执行中重入调用/撤销被拒绝（provider_busy / false）；不得在同步调用内卸载自己。Unregister 先 Quiesce（停止接单、取消/等待自有异步工作、恢复自有临时修改）再撤销 feature，之后才能释放对象；未注册对象撤销返回 false。provider 模块 Shutdown 必须调用 Unregister。当前 API 不支持跨线程持有调用或热卸载任意正在运行 DLL。

工具插件不加载 AutoBind/ModKit 业务，也不提供其正式 provider。扩展 provider 模块只在 Editor 且接口实际可用时编译/加载；Optional 插件声明不能替代 UBT 条件依赖验证。0.2 配置启停承诺为重启生效，不承诺用户 UI 热切换。

## Commandlet 跨进程桥（A/C/E 线）

```text
UnrealEditor-Cmd.exe Example.uproject -run=SoloASProviders -RequestFile=<absolute-path> -ResponseFile=<absolute-path> -unattended -nocrashreports -NoMessaging -NoASDebugServer -NullRHI
```

调用方用参数数组，设硬超时，使用每请求独有的输出文件并检查进程退出；不要从 UE stdout 解析 JSON，不复用旧输出文件。请求必须是 1..1048576 字节有效 UTF-8 JSON，输入输出绝对路径且不能相同。输出为无 BOM UTF-8 JSON，信封同 CLI（无 command 字段）。进程退出：0 成功；2 传输/文件错误（不保证输出）；3 请求或 provider 错误（有结构响应）。新进程加载该工程启用的 Editor provider；它不查询另一个正在运行的编辑器，也不把历史快照当可用 provider。

请求：

list 的 data 包含 packagingProviders、mcpProviders（可为空）和 restartRequiredForSettings=true。每个 provider 有 providerId、version、steps 或 tools。字段与上述值类型一致，JSON 使用小驼峰；MCP 工具使用 inputSchema、readOnly、destructive。

cancel.kind 只能 packaging/mcp。jobId 非空；由调用方控制，仅传给 provider。未知 provider/step/tool 分别报 provider_missing/step_missing/tool_missing；未知 operation 报 unknown_operation；坏版本/必需字段报 invalid_request。缺失可选 provider 的 list 是空集合；调用一个必须 provider 时失败，不能返回空成功。通用 stdio Host 已接入本桥；长时 UE provider 调用的跨进程异步取消、常驻 UE 桥和 AutoBind API 迁移仍待实现。新启动一个 commandlet 的 cancel 不会取消已结束进程或另一个进程的内存任务。

## 验证入口

```text
python -m unittest discover -s Build/PackagingTool -p test_*.py -v
Automation RunTests SoloAS.DeveloperTools.Providers
```

生命周期测试使用独立双接口替身，覆盖注册、重复冲突、执行、取消、撤销、旧调用拒绝、相同 ID 新实例再注册、两种发现顺序与协议负例。正式外部 provider 的取消、卸载与缺工具构建矩阵由接入方单独验证。


## v3 兼容策略

C++ `SoloASDeveloperTools::ContractVersion` 与 Python `CONTRACT_VERSION` 均为 3，描述公共接口修订；JSON 的 `schemaVersion` 仍为 1。Provider 请求仍接受且仅接受 schemaVersion=1，响应新增 contractVersion=3；provider 的虚函数、值类型和 feature 名保持 v1。旧消费者须忽略未知响应字段，不能将 ContractVersion 当作 wire schemaVersion。既有有效配置、方案、job 无需结构迁移；job 的 contractVersion/log 新增字段可缺省。旧命令的参数和默认同步 run 保持兼容；活作业 cancel 从未实现错误升级为持久取消请求，返回成功只表示请求已送达。

v3 明确原先未规定的 extraFiles 路径基准为项目 Content，新增绝对路径与父级遍历校验；不替旧配置自动加减 Content 前缀。已有 Content 相对配置、方案和 job 无需迁移，曾依赖绝对路径或越界路径的输入现在返回 invalid_config/exit2，须先改成 Content 相对路径。v3 增加未集成工具链的 unavailable 分支；消费者应先判断 toolchain 是否含 selection，再消费 F 的实探字段，unavailable 必须显示 pending，不能显示已通过。Provider ABI、schemaVersion=1、既有 F 探测响应与 job 状态机不变。

后续仅新增可选字段时增加 ContractVersion，维持 schemaVersion；删除/重命名字段、改变字段类型或必需语义时另增 schemaVersion 并显式拒绝不支持的输入。v3 不引入任意 provider DLL 热卸载或运行时设置热切换承诺。

## 项目设置与方案写接口（D/E）

config write 省略 --platform 时写 Config/DefaultGame.ini；指定时只写 Config/<platform>/<platform>Game.ini。只接受已映射的 target/configuration/pak/ioStore/maps/cookDirectories/extraFiles；其他工具设置须存方案，返回 setting_not_in_ini。数组是完整替换列表，空数组写 UE ClearArray；重复值保留。数组值中的引号/换行拒绝，避免产生非法 ini。只重写实际变更的键，移除该键在目标文件内各 section 的旧操作，再在末尾同名 section 写权威值；其他行、独立注释和行内注释内容保留，UTF-8 BOM 与 CRLF/LF 风格保留。仅支持 UTF-8 ini，不转换未知编码。没有变更时不写文件。写入为同目录临时文件原子替换。

返回 data={path,saved,changed,diff}，path 为项目相对路径；diff 是 unified diff，--dry-run 不写入且 saved=false。UI 应先预览，再让用户确认保存；确认时重新调用不带 --dry-run，重新读取当时文件；同一文件不支持多写入者同时保存。保存默认 ini 后，平台 ini 仍可覆盖它，config read --platform 才是该平台预览。这里只维护本合同列出的项目覆盖，不声称完整 UE 配置层级。

profiles save 保留其他方案，新增名称可直接保存；已有名称缺 --replace 返回 profile_exists/exit2，覆盖必须显式 --replace。返回 data={name,description,overrides,saved:true,path:"Config/SoloASPackagingProfiles.json"}。保存方案不改项目 ini；profiles JSON 按 schema 重排，无注释保留承诺。

Python 对应函数为 config.save_project(project, changes, platform=None, dry_run=False) 与 config.save_profile(project, name, overrides, description='', replace=False)；UI/MCP 统一经 CLI 调用，不嵌入 Python 解释器。

## 后端委托与分步操作（C）

CLI 只加载与入口相邻的 packaging.py，不导入系统同名 PyPI 包。文件缺失报 backend_not_implemented；ImportError 报 backend_import_failed（含缺失依赖）；函数缺失报 backend_interface_missing，均 exit4，不能返回空成功。

| CLI | packaging.py 公共函数 | 返回 |
| --- | --- | --- |
| run | run_job(project: Path, resolved: dict) | 完整 job dict；仅 succeeded 为 exit0，failed/cancelled 为 exit3 |
| clean-cache | clean_cache(project: Path, resolved: dict) | {ok:bool,error:null或{code,message},...} |
| generate-precompiled | generate_precompiled(project: Path, resolved: dict) | 同上 |
| sync-jit | sync_jit(project: Path, resolved: dict) | 同上 |
| check-package --path <dir> --mode auto\|source\|bytecode\|semi_aot | check_package(project: Path, path: Path, mode: str) | 同上；data 应含识别模式与不一致原因 |

分步操作一一对应 dev packaging 的清理旧缓存、生成预编译数据、同步 JIT、检查包。前三项解析同一 --profile/--platform/--overrides；check-package 接受现存包目录的绝对化 Path，mode 默认 auto，不解析 profile。分步返回 ok=false 为 exit3，输入/I/O 错误为 exit2。后端业务日志只追加 job.log 或自身日志文件，不 print 到 CLI stdout。

resolved 保留 v1 配置字段，可新增 jobId（来自 run --job-id）。C 线必须调用 create_job(project,resolved) 或实现同等合同：采用指定 ID、不覆盖旧 job、先持久化 input.json 和 queued，再进入实际工作。create_job 返回 (directory,job)；每次 run 默认新随机 ID，显式 ID 必须是 32 位小写 hex，重复返回 job_exists/exit2。input.json 永不改写，job.configDigest 仍只标识 effective 配置。C 线负责记录完整原生输入摘要，不借本合同冒充同源配套。

## 窗口调用、日志与取消（D/C/E）

**选择 C++ 侧启动 Python CLI 子进程并读取 job 文件，窗口不经 commandlet 执行打包。** 配置/方案/status 不需要 UE 冷启动；所有入口复用同一 Python 合并/保存/状态机，窗口保持响应，子进程拥有打包执行。仅 provider 执行与工具链探测使用对应 commandlet。

D 用 FPlatformProcess 的进程 API 启动已配置的 Python 可执行文件，传入 CLI 路径与参数；每个参数独立正确转义，不能拼 shell。短命令消费 stdout 的单个 JSON 信封并检查退出码；stderr/解析失败要显示诊断。长命令由 UI 生成 GUID 的无连字符小写 32 hex 字符串，调用 run --job-id <id>，持续持有进程句柄，在后台等待并设硬超时。UI 可在 run 尚未返回时每 250–1000ms 调 status --job <id>/logs --job <id>；job 尚未创建时 job_missing 是可重试的启动窗口，CLI 进程结束仍无 job 则失败。取消后继续持有句柄，等清理和终态后再释放，不能仅杀 UI 线程当作已取消。

status data={job,cancelRequested}，job.phase 为 PHASES 中的阶段或 null；job.history 可用于展示进度。job.log 为无 BOM UTF-8 追加文本，由单一 executor 写入；CLI stdout 始终只输出最终响应。logs --job <id> --offset <byte-offset> --limit <bytes> 返回 data={text,nextOffset,eof}，默认 offset=0、limit=65536，范围 offset>=0、limit=4..1048576。只能沿用返回的 nextOffset（字节偏移，不是字符数），跨读取的 UTF-8 字符留到下一次，eof 只表示当前文件尾，不表示 job 结束。旧 v1 job 无日志文件时返回空文本。直接读文件时遵循同样的原子 job 快照/追加日志协议。

cancel --job <id> 对非终态原子写 cancel.request.json={schemaVersion:1,jobId,requestedAt}；重复请求幂等，返回 data.cancelRequested=true、job 为当前快照，不抢 executor 写权。C executor 使用 cancellation_requested(directory) 或检查该固定文件，在启动前、阶段边界、等待子进程循环中轮询；停住自有进程树、恢复临时变更后才设置 cancelled。running 先进入 cancelling 再 cancelled；queued/prechecking 可直接 cancelled。请求与成功终态竞争时允许任务先完成，UI 以最终 job.state 为准。终态 cancel 不修改状态/文件，不承诺撤销已交付产物。

## 窗口菜单接入（D）

公共头 `Source/SoloASDeveloperToolsEditor/Public/SoloASDeveloperTools.h` 导出 `bool SoloASDeveloperTools::OpenPackagingWindow()` 与 `bool IsPackagingWindowAvailable()`；固定 tab ID 为 `SoloAS.ProjectPackaging`。Editor 启停对称注册/撤销 nomad tab spawner，SoloAS → 项目打包经 HasTabSpawner/TryInvokeTab 打开。commandlet 不打开窗口；缺窗口实现时菜单禁用、函数返回 false。模块通过 `__has_include("PackagingWindow/SoloASPackagingWindow.h")` 条件接线，Startup 仅在 !IsRunningCommandlet 时注册。

## 工具链 precheck 接入（F）

SoloASDeveloperToolsToolchainEditor 已集成为 Win64-only Editor 模块（Default 阶段）。Mac 缺探测能力保持 pending。

```text
python soloas_package.py --project <uproject> precheck --editor <absolute-UnrealEditor-Cmd.exe> --json
UnrealEditor-Cmd.exe <uproject> -run=SoloASToolchain -ResponseFile=<absolute-unique-file> -unattended -nocrashreports -NoMessaging -NoASDebugServer -NullRHI
```

只读 commandlet 输出无 BOM UTF-8 JSON，采用 F 线现有平铺格式，**不是 CLI 的 ok/data/error 信封**：

schemaVersion 必须为整数1，platform 为 Win64，preference 为 Auto/ForceOwn/ForceManaged，selection 为 own/managed/missing，restartRequiredForSettings 为 bool。F 可附加探测细节，客户端忽略未知字段。退出码 0=可用，selection 必须 own 或 managed；3=不可用，selection 必须 missing；2=参数或文件 I/O 错误，不保证文件。precheck 读取 selection 和退出码，不要求 F 额外输出 ok/error。selection=missing 汇入 toolchain_unavailable 错误。探测器内部超时180s，Python 客户端硬超时420s，预留 UE 启动时间。不得安装/卸载、切换用户工具链或保存项目设置；临时探测文件/子进程由 F 清理。

Python 以参数数组启动，每次探测生成独立的 32 位小写十六进制 jobId，将 ResponseFile 与 stdout/stderr 分别保存在项目 Saved/SoloASDeveloperTools/Logs/<jobId>/response.json 和 probe.log。成功、失败和超时均保留该目录，CLI 不在退出时删除，以避免引擎子进程持有日志句柄时的 Windows 共享冲突；日志保留不改变响应 schema 或 ContractVersion。只读取文件 JSON 并校验退出码与 selection 一致。precheck 先读取入口所属插件的 SoloASDeveloperTools.uplugin Modules；未声明 SoloASDeveloperToolsToolchainEditor 时不启动 commandlet，即使传入 --editor 也返回 data.toolchain={"status":"unavailable","reason":"module-missing"}，pendingChecks 保留 toolchain，缺模块本身不加入 errors、不使 configurationValid=false、不使 CLI 失败。readyToPackage 仍为 false；其他真实配置/provider 错误照常失败，不能因缺 F 模块被吞掉。描述符是集成开关，不依赖 Source 目录是否存在，以支持预编译插件。

模块已声明时，precheck.data.toolchain 保存整个实探响应；未显式 --editor 时为 null 且 pendingChecks 含 toolchain；实探后移除该 pending，exit3 使 configurationValid=false，CLI exit3。探测 exit2、坏 JSON、版本不支持/退出码不一致、超时为 CLI exit2；不把它们降级成工具链已通过。即使探测可用，readyToPackage 仍为 false，其他 pending 项由 C 接入真实检查后再放行。

## R2 集成

三个 Editor 模块默认接通；固定窗口 ID 为 `SoloAS.ProjectPackaging`，其他插件可通过 `FGlobalTabmanager::TryInvokeTab` 打开，无需链接依赖。InputCore 属于 Editor 模块的私有引擎依赖。工具链缓存与托管目录使用运行引擎的 `FEngineVersion::Current()`；安装计划保留已固定的 Microsoft 工具版本，最终可用性仍由该引擎 UBT 判定，不按引擎补丁版本字符串放行。安装器真实下载/安装不在本轮验证范围。

打包后端通过工程 EngineAssociation 查找已注册安装引擎，不固定引擎补丁版本。成功 job 的 `result.outputDirectory` 相对开发工程，`manifest` 为 `SoloASPackage.json`，`logs` 为进程回执路径；`releaseDirectory` 是工程父目录下的独立发布副本叶名。source 包含脚本源码及绑定数据库；bytecode/semi_aot 需要同批三基础 cache 与声明的 companion。静态分类与 `runtimeVerified=false` 不授予运行资格，运行证明单独读取。`clean-cache`、`generate-precompiled`、`sync-jit` 经同一 run_job 的 stop_after 分步执行；完整 run 保留全部阶段。

旧接线说明及重复协议示例见[原字节摘录](../../../docs/archive/docgov-r4/DeveloperTools-Contracts-excerpts.md)。
