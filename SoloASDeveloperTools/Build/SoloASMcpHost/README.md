# SoloAS MCP Host

通过 stdio 提供通用工程信息、AS 诊断/调试、打包 CLI 路由和 Editor provider 工具。产品依赖仅 Runtime 与 Bridge；不需要安装 AutoBind 或 ModKit。设置修改后重启 Editor，并重启客户端 Host。AutoBind 可选注册 `autobind.api_catalog`，提供目录快照状态和元数据位置。

## 构建与启动

开发者使用 .NET 8+ SDK 和 Python 3.10+：

```text
python Build/SoloASMcpHost/publish.py --runtime win-x64
```

发布输出为插件的 `Binaries/ThirdParty/SoloASMcpHost/Win64/`。整个目录一起分发，包含 Host、自包含 .NET 运行库与依赖；用户无需安装 .NET。首次构建从官方 NuGet 恢复 SDK/运行库包，无第三方库依赖。Python 仍需单独安装，用于公共打包 CLI。

```json
{
  "mcpServers": {
    "soloas-angelscript": {
      "type": "stdio",
      "command": "<SoloASDeveloperTools>/Binaries/ThirdParty/SoloASMcpHost/Win64/SoloASMcpHost.exe",
      "args": [
        "--engine", "<UERoot>", "--project", "<Project.uproject>",
        "--lane", "stock", "--debug-host", "127.0.0.1", "--debug-port", "27099",
        "--python", "<Python executable>", "--timeout-ms", "2700000"
      ]
    }
  }
}
```

`--engine` 接受引擎根或 UnrealEditor-Cmd 可执行文件；`--project` 是具体 .uproject。默认 Python 为 Windows 的 python、其他平台的 python3；空格路径使用客户端参数数组，无 shell 拼接。默认单请求超时 120 秒，可设置 1 秒到 90 分钟。完整打包须按实际预算增大超时。Host 不开启服务器监听端口，只连接已有的 AS DebugServer；UE 官方 MCP 独立运行。Mac/Linux 发布 RID 有构建入口，未授予编译/运行资格。

## 工具与路由

| 工具 | 行为 |
| --- | --- |
| as_api_search / get / list / explain / incremental / changes | 读取 DebugServer 已注册类型表；按签名附加 AutoBind 等级、标签和来源元数据；离线读取上次完整类型表 |
| autobind.api_catalog | AutoBind 可选数据注册点，返回当前目录快照状态及可移植元数据位置 |
| as_project_info | 工程、引擎版本、描述符中明确登记的插件/模块、脚本根、通用 AS ini、targets/maps、连接与诊断状态；不是 UE 全量插件解析或编译状态机 |
| as_diagnostics | 当前连接推送的编译错误/警告，携带 connected；断线时不能以空诊断判断编译成功 |
| as_debug_set_breakpoint / clear_breakpoints | 设置/清除断点；set 返回 accepted，不伪造 verified |
| as_debug_continue / step | 继续或单步；step.kind 为 in/over/out |
| as_debug_stack / variables | 暂停时读取调用栈、指定 path 的变量 |
| as_debug_eval | 在 frame 求值 expression，可能产生副作用 |
| soloas_package_profiles / profiles_save | 列方案；保存方案传 name/overrides，可选 description/replace，覆盖已有名称须显式 replace=true |
| soloas_package_config | config read，可选 profile/platform/overrides；不保存配置 |
| soloas_package_config_write | 传 overrides 和必需的 dryRun；true 预览精确 diff，false 显式保存项目设置，可选 platform |
| soloas_package_precheck | precheck，可选 editor 实探工具链；检查 readyToPackage/pendingChecks，配置有效不等于可打包 |
| soloas_package_run | 默认同步 run，可选 jobId 供其他会话查询/取消，保留 data.job 与真实失败；写入/构建属性明确标注 |
| soloas_package_status / cancel | 传 job（32 位小写 hex），查询历史/请求取消；读取失败终态本身是查询成功 |
| soloas_package_logs | 传 job，可选 offset/limit；字节偏移沿用 nextOffset，eof 不代表 job 完成 |
| soloas_package_clean_cache / generate_precompiled / sync_jit | 统一路由清缓存、生成预编译数据、同步 JIT；真实执行由公共后端提供 |
| soloas_package_check_package | 传 path，可选 mode=auto/source/bytecode/semi_aot；包判定由公共后端提供 |
| `<providerId>.<tool>` | 每次 tools/list 重新枚举启用的 Editor provider，调用经公共有界文件桥，不保留 DLL 指针或历史工具列表 |

核心工具参数沿用旧 Host；打包参数严格映射 [公共合同](../../Docs/Contracts.md)。config/precheck/run 与前三个分步操作支持 overrides JSON object；profiles 列表不接收参数。config_write 必须显式选择 dryRun，profiles_save 的 replace 默认为 false。配置/方案保存只经公共 CLI，不在 Host 写 ini。stdout 只输出每行一个 JSON-RPC 2.0 对象，诊断走 stderr，UE 日志和每请求原件在 `Saved/SoloASDeveloperTools/Mcp/requests/<随机ID>/`。

provider 路由复用 `-run=SoloASProviders`。每请求独立 request/response 文件（请求至多 1 MiB，后端响应文件至多 16 MiB）、检查 UE 退出码和信封；缺 provider 返回 provider_missing，发现失败不伪造空列表；工具 annotations 保留 readOnly/destructive。新 commandlet 加载当前工程启用的 provider，不能访问另一个 Editor 的内存。读取编辑器实时 AS 数据使用 DebugServer。

Host 顺序处理请求；同步打包期间，该 stdio 会话的后续 status/cancel 等待前一请求结束。调用方可先生成 32 hex jobId 传入 run，再从另一个 Host/CLI 查询或取消。cancel 成功只表示取消请求已持久化；实际停止/恢复与终态取决于打包后端。超时停止并等待 Host 自己启动的进程树，不代表恢复打包后端未实现的事务或撤销另一个进程的异步 provider。当前公共 ABI 是同步调用，不承诺跨 commandlet 进程取消后台任务。

## 从旧启动方式迁移

1. 将旧启动配置的 command 改为 DeveloperTools 的 `Binaries/ThirdParty/SoloASMcpHost/Win64/SoloASMcpHost.exe`，engine/project/lane/debug-host/debug-port/timeout-ms 参数沿用；旧 AutoBind Host 源码和独立 commandlet 已退役，不自动写用户 `.mcp.json`。
2. 六个 `as_api_*` 工具保持原名称、联合目录、分页、变更和游标语义。AutoBind 自动输出 `Saved/AngelscriptAutoBind/Mcp/runtime-api-metadata-v1.json.gz`，唯一 Host 只按运行时签名附加元数据，不把元数据文件里的独有符号补入类型表。
3. 新通用类型表快照位于 `Saved/SoloASDeveloperTools/Mcp/debug-database-v1.json`。首次在线成功收表后保存新快照，刷新未完成时沿用旧 Host 的上次目录历史回退；离线快照不标记为 live/typeTableReady。首次离线且没有快照时，返回 `DebugServer runtime type table is not available yet`。新路径不存在时，只读回退 `Saved/AngelscriptAutoBind/Mcp/debug-database-v1.json`；不会修改旧快照，后续在线成功收表只保存新路径。
4. DeveloperTools 是 AutoBind 的可选 Editor 依赖；未安装或禁用时 AutoBind 正常加载并继续提供自身能力，不注册 `autobind.api_catalog`。Host 的 API 类型表查询本身不依赖 AutoBind，缺元数据时明确保留 unknown 状态。

## ai-assisted.md 承诺对照

对照网站 dev 的中文 AI 辅助开发页（f0b79e0d）。

| 承诺 | 当前实现与缺口 |
| --- | --- |
| 预编译 Host，用户无需 .NET | Win64 自包含发布入口与实际产物已具备；发行清单/签名由集成处理，其他平台待验 |
| 菜单复制配置 | 旧 AutoBind 菜单未改；新插件菜单接入由主控处理 |
| 在线/离线 API 查询、首次无快照错误、元数据和差分/游标 | 由唯一 Host 提供六个 as_api_* 工具；元数据来自 AutoBind 可选注册点 |
| 列出已编译脚本类型 | 使用 as_api_list，view=types，origin=as |
| 工程/插件/脚本根/连接状态/编译诊断 | 已迁移通用实现；不包含 AutoBind 选择解析与 ModKit 策略，不据连接状态推测正在编译 |
| 调用栈/变量/断点/继续/单步/求值 | 通用客户端已迁移，副作用注解已补齐；协议解码与离线拒绝有回归，真实暂停调试仍需代表场景资格 |
| 与 UE 官方 MCP 并用 | 两协议独立；本 Host 不新增网络服务，不替官方 MCP 操作资产/场景 |
| AI 配置 AutoBind/FeatureMod/ModKit 白名单 | AI 自行改项目配置的流程；AutoBind 只注册只读目录数据工具，不授予 Mod 调用权限 |
| MCP 只读信息和调试，不写配置 | 信息/调试保留；新增打包 run/cancel 有副作用，config_write/profiles_save 也会保存配置；旧文档这句须由文档 owner 更新。新接口明确区分 diff 预览和显式保存 |
| 原生绑定生成后编译并重启 | 合同保持；工具本轮不调用 AutoBind 生成器，设置只承诺重启生效 |

三种交付方式只由公共打包后端判定。基线后端尚未实现，MCP 不能把返回 job、缓存存在或错误退出变成三模式包验收通过。

## 验证

协议/路由测试无外部测试框架与测试监听器：发布 `Tests/SoloASMcpHost.Tests.csproj`（同样 net8.0、自包含），运行 `SoloASMcpHost.Tests <Host executable> <Python executable> <output directory>`。受控子进程替身覆盖传输、非法请求、通知无副作用、重复工具名、provider 超时与进程树、CLI 三模式失败/历史终态、精确 diff/保存/方案替换、jobId/logs 与分步接口；替身不授 UE provider 资格。

真实 Editor selector：`Automation RunTests SoloAS.DeveloperTools.Mcp`。真实 Host→UE provider→结果的端到端验证需在只安装 Runtime+Bridge+工具的工程里注册测试 provider，核对列表/调用与撤销后的拒绝。实际回执范围见本轮交付报告。
