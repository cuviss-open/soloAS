# AngelscriptModKit SDK 性能纪律（SDK-Performance.zh.md）

> 配套 `Docs/SDK.zh.md`。性能纪律**不改引擎、只立纪律**：mod/补丁作者必须
> 遵守下面的边界；系统侧只提供解释执行耗时统计手段（G5 预算门），并诚实
> 声明能力边界（无 per-function profiler）。与预检的分工：预检判结构、性能
> 靠纪律 + 实测。

## 1. Tick 级热点纪律

- mod/补丁**禁止挂 Tick 级热点 override**（如 `Tick`）；确有必要时先给出
  预算上限并实测达标再挂。
- 解释执行 + 无 JIT（JIT 产物必须编进二进制）的前提下，热点代码补丁要控制
  规模，修 bug/调逻辑型补丁才是热更的正当场景。
- 纪律同 Lua 热更：**只改函数体/逻辑，不动类布局**。结构变化补丁在预检
  （`ModKitPatchPrecheck`）即被拒收，只能随下次发版；运行时 SoftReloadOnly
  对 Required 变化拒绝并保留旧代码，Suggested 变化则已软换入并记录 `applied-partial`；R1 路径下次启动会从源码全量编译、结构变化
  静默生效——预检拒收是唯一的闸。

## 2. 预算门配置（ModScriptExecutionTimeoutSec）

唯一的解释执行耗时统计与失控保护手段 = G5 预算门
（Runtime 注册点 `CS.Runtime.Modding.ScriptExecutionBudgetGate` + ModKit
handler `Private/ModKitScriptExecutionTimeout.cpp`）。wall-clock 预算：

- `ModScriptExecutionTimeoutSec`（秒，ClampMin=0.1，默认 5.0）：一次脚本调用
  超预算即中断（stock 异常路径）。**Cooked（Test/Shipping）构建不装内置
  loop-detection 回调，ModKit 预算门是唯一的执行预算**。
- Editor 构建自带内置 1s 回调（`UAngelscriptSettings::EditorMaximumScriptExecutionTime`），
  ModKit 预算在此返回 0（不重复安装，Editor 无效化该预算）。
- Development 构建也带内置回调；需要验证 ModKit 预算门时开
  `bForceModKitTimeoutInDev`（force 开关，发射 `MODKIT_TIMEOUT_FORCE_ACTIVE`；
  默认 false = bit-for-bit 上游行为）。
- 连续超时计数：`bDisableModOnTimeout`（默认 true）+
  `MaxConsecutiveTimeoutsBeforeDisable`（默认 3）→ 连续超预算后
  `MODKIT_MOD_DISABLED reason=script-timeout`（游戏继续启动/运行，坏 mod
  不拖垮全局）。
- mod 启动激活独立超时 `ModStartupActivationTimeoutSec`（每 mod 各自 deadline，
  超时以 `MODKIT_MOD_DISABLED` 禁用）。

配置示例（`<Project>/Config/DefaultEngine.ini`）：

```ini
[/Script/AngelscriptModKit.ModKitSettings]
ModScriptExecutionTimeoutSec=5.0
MaxConsecutiveTimeoutsBeforeDisable=3
```

## 3. 归因日志（ANGELSCRIPT_EXECUTION_TIMEOUT）

超预算时按模块归因（`elapsed` 为秒）：

- `ANGELSCRIPT_EXECUTION_TIMEOUT module=<模块名> elapsed=<秒>`：mod 模块超预算。
- `MODKIT_SCRIPT_TIMEOUT_UNATTRIBUTED module=<模块名> elapsed=<秒>`：非 mod
  模块（游戏主体 / DevOnly / 未归属）只记日志，不计入禁用预算。
- `MODKIT_TIMEOUT_COUNTED mod=<ModName> count=<连续次数>`：连续计数，仅
  `FeatureMods.<ModName>.*` 叶子模块计入；达到阈值 → `MODKIT_MOD_DISABLED`。

归因是**模块粒度**：定位到超预算的 mod 后，再用分模块拆解定位热点函数。

## 4. 无-profiler 诚实边界

- **旧 UE-AS fork 的历史限制（待清理，不再维护）**：该分支无 per-function profiler；无 SCRIPT_PROFILING 开关、无
  `GetTimeUsage` / `GetProfile`、无 asEP profiler 属性（引擎属性枚举无
  profiler flag）。因此 SDK 不提供逐函数计时。
- 预算门（§2/§3）是**唯一**交付态的解释执行耗时统计手段。
- 热点头定位 = 超预算归因 + 分模块拆解 + 代码走查，与 §1 纪律配合使用。
- 提示：Development 构建安装行回调（debug server / line callback），有额外
  解释开销；性能评估以 **Test/Shipping** 为准。
- 编译管线计时（`FAngelscriptScopeTimer`）是原生编译路径耗时，**不是**脚本
  解释耗时，勿混用。

## 5. 与预检的分工

- 结构变化（Suggested/Required 同档）→ `ModKitPatchPrecheck` REJECT，随下次
  发版。
- 性能问题预检无法拦截 → 预算门统计 + 上线前 Test/Shipping 实测。
- 两者共同构成 mod 质量闸；预检在 ops 侧源码模式早闸，运行时 SoftReloadOnly
  对 Required 变化拒绝，Suggested 变化按热更文档 §1 报告 `applied-partial`。

预算在 VM 的循环检测回调采样点检查；长时间阻塞的 native API、JIT 本体与内存耗尽不在此打断保证内。
白名单应只开放有界原生操作，不把脚本预算当成进程沙箱。当前“连续”超时计数实际按会话累计，
成功调用不会清零；达到阈值标记 Mod 失败，不强制销毁仍被游戏持有的对象。
