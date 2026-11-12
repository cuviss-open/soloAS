# AngelscriptModKit 热更通道（G10）

> 本文说明客户端热更补丁（hot patch）的制作、验签、下发与显式触发纪律。
> 入门与预检工具见 `Docs/SDK.zh.md`，性能纪律见 `Docs/SDK-Performance.zh.md`。
> 全部配置项见 `Source/AngelscriptModKit/Public/Core/ModKitSettings.h`。本文描述首版（G10）交付形态。

## 1. 通道总览（H1/H2/H3）

热更通道由拉取、落盘和显式换入三段构成；前两段失败关闭，换入结果按 Runtime 分类器记录：

1. **H1 拉取**：从补丁 feed 读取 manifest + 负载文件，全程在内存中完成，
   不落任何字节。feed 形态：本地目录 / `file://`（运营/CI 与验证链）或
   `http(s)://`（生产，HTTP 模块，超时与大小上限受配置约束）。
2. **H2 落盘**：落盘前对内存中的补丁**完整重跑验签链**（manifest schema →
   公钥 → payloadSha256 锚点 + Ed25519 签名 → 逐文件 SHA-256 → 预检
   PASS → 引擎版本匹配），随后从 `.tmp-incoming` 目录换入暂存根。
   验证或写 incoming 失败保留旧目录；替换时先将旧目录重命名为 `.previous`，
   再将 incoming 重命名到最终目录，成功后删除旧目录。换入失败会恢复旧目录；
   若恢复也失败，保留 `.previous` 并拒绝后续替换，需先恢复该目录。
3. **H3 显式触发**：`ModKit.Patch.Apply <PatchId>` 控制台命令（或
   `FModKitHotPatch::RequestApply`）在游戏线程顶层 tick 执行：再次对
   **暂存落盘字节**全量重验（落盘后篡改 = 拒绝，绝不进入编译），
   先将已接受版本写入临时文件、完整刷盘（fsync / FlushFileBuffers），
   再原子重命名发布水印；
   持久化失败在编译和换入前返回 failed，旧代码继续生效。随后以
   `CompileModules(SoftReloadOnly)` 编译并换入，成功才标记 applied；Runtime 分类器是唯一 verdict 源。
   水印持久化失败、预处理/编译错误及 `structural-required` 拒绝保留旧代码；
   失败补丁记 failed，本会话不再重放。`unexpected-suggested` 表示 Runtime 已软换入，
   ModKit 记录 `applied-partial` 并发出 `MODKIT_PATCH_APPLIED_PARTIAL`，水印保留且本会话重复触发
   返回 `already-applied`。新函数体已生效，但新增反射属性/元数据尚未完整更新；该状态不记 failed。
   遇到该状态应重启恢复基线，发布仅含函数体修改的修复补丁。

## 2. 配置项（ModKitSettings）

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `bEnableHotPatch` | `false` | 热更总开关，默认关闭（fail-closed）；关闭时落盘/触发一律 `reason=disabled` |
| `PatchPublicKeyBase64` | 空 | 32 字节 Ed25519 公钥（RFC 8032 点编码）的 Base64；私钥永不进入产品/包体 |
| `PatchVerifyKeyId` | `patch-verify-v1` | 公钥登记名（G6 keystore 先例） |
| `PatchAcceptedKeyIds` | 空 | 外部 FeatureMod 包签名可接受的附加 KeyId；共用同一 `PatchPublicKeyBase64` 信任根 |
| `PatchFeedUrl` | 空 | 运行时拉取入口的默认 feed |
| `PatchStagingRoot` | 空 | 暂存根覆盖；空 = `<ProjectSaved>/ModKitPatches`；拒绝与脚本扫描根、FeatureMods 或项目插件目录互相包含的覆盖值 |
| `PatchMaxBytes` | 64 MiB | 拉取总字节验收上限（manifest+负载），超限 `reason=size`；HTTP 接收流在追加每块数据前检查剩余额度，超限即终止请求；上限须为 1 至 2 GiB 以下 |
| `PatchFetchTimeoutSec` | 30 | http(s) 整个 feed 共用的总等待预算（秒），不是逐文件重新计时 |
| `bKeepFailedPatchOnDisk` | `true` | 触发失败后暂存目录保留取证；置 false 则删除 |

## 3. 补丁制作与签名纪律

- 补丁 = 一个目录：`Manifest.json` + 负载 `.as`（补丁相对路径即脚本相对
  路径，如 `MixedCompileCacheActor.as`）。
- manifest 格式 `ModKitHotPatchManifest/v1`：`patchId` / `version` /
  `engineVersion` / `files[]`（path + sha256）/ `precheck` / `signature`
  （`algorithm: ed25519`、`payloadSha256`、`value`）。
- **签名纪律**：签名覆盖确定性 canonical 串，包括 `signature.algorithm`、
  `signature.signer` 和 `signature.publicKeySha256`；`payloadSha256` 与签名值本身不参与 canonical；
  私钥只存在于离线签名工具链，不向客户端或作者 SDK 分发；产品代码零私钥、零引擎 crypto 依赖
  （Ed25519 verify-only 自包含实现，TweetNaCl 派生 public domain）。
- **预检纪律**：下发前必须离线 `ModKitPatchPrecheck` 取得 PASS 并写入
  manifest；结构变化（增删 UPROPERTY/UFUNCTION、改签名）一律拒收，随下次
  发版。预检被绕过时，运行时分类器对 Required 结构变化拒绝并保留旧代码；
  Suggested 变化则已软换入，按 §1 记录 `applied-partial`，不能保证旧代码保留。
- **引擎版本**：manifest 的 `engineVersion` 必须与运行引擎
  `FEngineVersion::Current()` 完全一致（`{主}.{次}.{补丁}-{CL} {分支}`），
  不匹配即 `reason=engine-mismatch`。

## 4. 运维操作

- 离线验签：`-run=ModKitPatchVerify -Manifest=<abs> [-PublicKey=<b64>]`，
  标记 `MODKIT_PATCH_VERIFY_OK` / `MODKIT_PATCH_VERIFY_FAILED reason=…`。
- 拉取+落盘（离线工具）：`-run=ModKitPatchLand -Feed=<url|dir>
  [-PublicKey=<b64>]`，成功标记 `MODKIT_PATCH_STAGED patch=<id> files=N`。
- 运行时拉取+暂存：`ModKit.Patch.FetchApply <feed>`。
- 显式触发：`ModKit.Patch.Apply <PatchId>`。成功标记
  `MODKIT_PATCH_APPLIED patch=<id> modules=N result=FullyHandled`；失败
  `MODKIT_PATCH_FAILED reason=…` + `MODKIT_PATCH_STATE state=failed`。
- 重复触发同一补丁：no-op，`reason=already-applied`；失败的补丁本会话
  拒绝重放（`reason=already-failed`）。
- **会话语义（v1）**：补丁仅会话内生效，重启不自动重放；持久化重放
  （`AppliedPatches.json` + 启动重放表）是 v2 开放项，未经裁决不要依赖。
- `version` 使用点分非负十进制整数（如 `1.0`），每段最多 9 位且不带前导零。
  同 PatchId 落盘不得降低已有版本；显式触发在编译前将已验签版本登记到
  `Saved/ModKitPatchVersion.txt`。水印表示“已被接受尝试的最高版本”，只升不降，
  编译失败或崩溃不会降低水印；任何低于水印的版本均被拒绝。恢复手段是发布更高版本，
  或对同版本修复、重新签名和落盘后重试；同版本允许通过版本检查。
  失败的 PatchId 在本会话仍为终态，重试同一 PatchId 需启动新会话。
  此检查不提供运行期内存完整性或对拥有本机写权限的攻击者的防回放保证。
- 编辑器/commandlet 上下文拒绝触发（`reason=editor`）；与编译/运行时
  操作重叠时拒绝（`reason=writer-held|busy`）。

## 5. 性能与安全纪律（重申 §3.3-4）

- 补丁负载走与启动追加相同的编译管线与分类器，无旁路；换入为软重载
  （Required 结构变化拒绝，Suggested 变化记录 `applied-partial`）。
- 默认暂存根不挂监听器；拉取失败/超时/超限不进入脚本编译。
- 开发模式轮询只扫脚本根，暂存根不被扫描（B6 门实证）；补丁只有经
  H3 显式触发才可能进入编译。

负载路径仅允许 ASCII 字母、数字、`_`、`-`、`.` 和分隔目录的 `/`，并以 `.as` 结尾；
拒绝绝对路径、盘符/ADS、空段、`.`/`..`、末尾点、Windows 保留设备名及大小写重复路径。
`patchId` 仅允许 ASCII 字母、数字、`_` 和 `-`，同样排除保留设备名。
清单拒绝未知字段；canonical 字段不得含控制字符或 `=`。

### HTTP 调用时机与可信暂存区

HTTP feed 同步等待；默认整个 feed 的网络等待预算为 30 秒，由 `PatchFetchTimeoutSec`
调整，manifest 与所有负载共用同一截止时间。轮询粒度为 10 ms；HTTP Tick、系统调度、
解析、验签和磁盘 I/O 耗时不属于可保证的实时截止时间。调用期间会阻塞调用线程。
仅在启动流程或用户显式调用 `RequestFetchApply` / `ModKit.Patch.FetchApply` 时使用；
队列只在有请求时注册顶层 tick，清空后移除，不自动轮询 feed。不要从游戏每帧逻辑提交请求。

暂存目录及其祖先只允许应用或安装器写入。检查时拒绝 Windows 8.3 短名、符号链接、
联接点和其他 reparse point，以及与脚本扫描根互相包含的路径。
这不是句柄级隔离：不防御已拥有暂存区写权限的本地攻击者在检查后替换目录或链接。
安装器必须维持这一权限边界，不能把共享可写目录用作暂存区。
