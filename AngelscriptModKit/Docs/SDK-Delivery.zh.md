# AngelscriptModKit SDK：交付形态

主体、普通插件/GFP、FeatureMod 使用原生源码根与 descriptor 身份选择交付输入。生成器保持原版 cache/JIT 行为；交付工具处理输入 stage、离线 envelope 和产物盘点。安装及 FeatureMod 使用见 [SDK 入门](SDK.zh.md)。

## 源根与生成集合

工作区入口为 `project/tools/ModKit/modkit_delivery.py stage --project <prepared-project-directory> --destination <new-stage-directory> [--engine <installed-engine-root>] [--leaf <FeatureMod-name>]`。输入是已准备工程，输出必须是独立新目录，源工程不变。工具按 `.uproject`、`.uplugin`、`FeatureMod.json` 的依赖构造 Win64 Game 闭包，处理 `SourceOnlyScriptPaths`，排除生成物与 keystore。外部插件 descriptor 从安装版解析；外部脚本根须由 canonical producer prepare 先本地化，未解析依赖、循环、主体/普通 GFP 反向依赖 FeatureMod 和跨目标条件不明均拒绝。

主体 stage 包含项目和启用的普通插件/GFP；`--leaf` 在此基础上加入指定 FeatureMod 及其依赖。这个完整生成输入仍包含主体，不是独立的 leaf cache 集合。cache 与 JIT 输入集合分别列出，当前两集合必须相同。`JitOnlyModulePrefixes`、核心输出重定向与旧 `as-cache-unit` 协议不再静默接受；任意 JIT 子集和独立 H5 集合交付保持阻塞。

stage 完成后沿用 canonical prepare 的业务初始化、绑定数据库及签名版本地板；stage 本身不提供这些运行资格。在独占的 standalone packaged producer 中调用原生 `-as-generate-precompiled-data -as-write-bind-db`。生成输出是实际 `FPaths::RootDir()/AS_JITTED_CODE`，启动日志 `MODKIT_JIT_ROOT root=<absolute-root>` 供工具核对；工作目录不能替代 RootDir。禁止使用安装版引擎目录作为生成输出，不改引擎文件，不允许多个作业共享输出位置。

## 产物盘点与配对

生成进程成功结束后，用 `modkit_delivery.py inventory --root <isolated-producer-root> --caches <producer-script-directory> --generation-log <actual-log> --build-log <actual-UBT-log> --output <jit-generation-manifest.json>` 盘点完整产物。构建日志须来自该输出的实际目标构建，包含输出绝对目录、全部生成 `.cpp` 编译单元和 `Result: Succeeded`，时间不得早于输入。工具检查内部 include 闭包、cache/JIT 原始 GUID、实际 `FStaticJITFunction::Register` 的 serialized ID 与重复/未知语法，并记录文件哈希。未能唯一解析的模块归属保留为 Unknown，不推导覆盖。

manifest 沿用 schema 2；`coveredModules` 和 `coveredFunctionIdentities` 保持空，`bytecodeStrippingQualified=false`。注册符号与文件存在不证明函数可删字节码。构建日志检查不能替代最终 Game 链接、CDO 构造和真实调用；交付还须由现有 runner 完成这些验证。cache 与 JIT 始终按同一生成批次整体交付，不改写 GUID，不把新 cache 拼接旧 JIT，不启用批次跳过或 strip/reuse。

## 整批复用设计

后续可复用已经验证的不可变生成批次：完整源码及其依赖闭包、配置、工具和引擎身份必须与原批次严格相同，并且重新核对该批次的完整输出清单和每个输出摘要。输入缺失、身份无法确定、文件增加或删除、摘要不符时均完整重建，不能只重建或拼接个别模块。批次以独立目录生成，验证 cache/JIT 配对及完整性后原子发布；发布失败不得覆盖仍在使用的旧批次。复用前后还须验证实际 Game 链接、重启和调用。当前工具尚未实现该复用流程，现有盘点结果也不覆盖完整输入闭包，不能单凭盘点一致跳过生成。

## 窄字节 envelope

`bEncryptScriptCache=true` 与 `ScriptCacheKeyId=<keyId>` 启用认证 envelope 消费。密钥只存在于 `Build/ScriptCacheKey.json` 或 `MODKIT_CACHE_KEY_PATH` 指向的 keystore，不进 ini、SDK 或公开仓库。工具 stage 不复制 keystore；发布方单独部署并限制访问。现有 `ModKit.GenerateScriptCacheKey -KeyId=<id> -ModulePath=<final-Game.exe> -RuntimeModule=<module>` 为最终消费二进制生成绑定材料，二进制变化后须重新绑定。默认 runtime 哈希提供者仍只有 Windows PE `.text`，其他平台的 runtime hash 等价保护未实现。

先核对完整明文 cache/JIT 配对，再离线封装为一个新文件：

```text
UnrealEditor-Cmd <Project.uproject> -run=ModKitSealScriptCache -Source=<verified-plain-cache> -Output=<new-envelope-file> -unattended -nocrashreports -NoMessaging -NoASDebugServer
```

该入口要求加密配置与有效密钥，拒绝同路径、覆盖已有输出和重复封装 MKCC；完整写入临时文件后才发布。失败保留原件。将已封装文件安装为 `Script/PrecompiledScript.Cache` 或相应 Development/Test/Shipping 变体，并用现有签名/交付清单绑定封装后的实际文件哈希。

插件在 PreDefault、AngelscriptLoader 之前安装只读文件层，四个精确缓存路径分别冻结完整认证快照。同步 FileExists/FileSize/OpenRead 返回同一快照的明文存在与大小，handle 支持有界 Read/ReadAt/Seek/SeekFromEnd/Tell/Size，各 handle 独立游标。非目标路径透传当前下层，保留 Pak/IoStore 链。目标 async/mapped 入口明确失败。认证失败、错误密钥、旧版本/旧 flags 或运行时完整性破坏不返回明文 handle、不回退同名密文或源码；off 下一进程不安装文件层、不读取密钥，保留原明文流程。

文件层、快照及模块保留到进程退出，buffer 最终销毁时清零。不支持带活 handle 动态卸载，不在 ShutdownModule 修改文件链或在运行中替换快照。MKCC v2 使用 AES-256-CTR 与 HMAC-SHA256，认证覆盖头部与密文，固定时间比较 MAC。加密用于检测篡改、误加载和误用，不承诺对拥有本机文件与进程控制权者保密。

## 尚未支持的交付策略

`bObfuscateOpcode`、`bStripLineNumbersOnCache`、`bStripBytecodeForJitModules` 请求时，生成/封装明确拒绝，不生成宣称满足这些策略的交付物。带 opcode/debug-strip 或未知 flags 的旧 MKCC 文件拒绝，不能作为 stock 原始 cache 解封后继续加载。精确字节码 strip、debug-strip、opcode permutation、任意 JIT 子集和精确 strip/reuse 都不由窄 envelope 或离线盘点提供。

工作区后续复用 G6 keyed/integrity、G7 module-filter/incgen 和 MK14/MK18 的现有夹具。旧脚本的核心 gate、输出重定向、跳批及剥离假设须先迁移，不能直接作为当前实现的运行凭证。最终交付须补真实启动加载、坏认证/错密钥/旧 flags 负例、Game 链接与调用、进程退出，按 5.4–5.8 和适用平台分别验证。

## 独立功能模组交付

管理器的 **Assemble** 将选中的功能模组复制到 `Saved/FeatureModPackages/<ID>-<唯一值>/`，不要求 DeveloperTools，也不打包主体工程。命令行入口为：

```text
UnrealEditor-Cmd <Project.uproject> -run=ModKitAssemble -Source=<prepared-mod-directory> -Output=<new-directory> -unattended -nocrashreports -NoMessaging
```

Source 的末级目录名与 `<ID>.uplugin` 一致。输出目录必须不存在且在 Source 外；排除 Binaries、Intermediate、Saved、.git，保留已准备的脚本、模块 cache 与伴随文件。assemble 不生成 cache、不 cook 资产；cache-only 交付先按 MK14/MK18 生成配对的 ModuleCaches，再去掉源码并 assemble。内容型功能模组仍须先完成适用平台的 cook。

GFP 依赖在 `FeatureMod.json` 的 `dependencies.branches` 中声明，并在 `.uplugin` 的 Plugins 中启用对应依赖；普通 GFP 必须有真实 GameFeatureData。已启用且可含内容的 GFP 可用 `-run=ModKitCreateGameFeatureData -Plugin=<GFP-name>` 创建初始资产；已有资产不会覆盖。功能模组可依赖 GFP 与树干；主体、普通插件和 GFP 不得反向依赖功能模组。

玩家包不需要签名，将输出的 `<ID>/` 复制到外部根 `FeatureMods/`。默认外部玩家模组仍受白名单和首次确认约束；自己的 `Plugins/FeatureMods/` 随主体交付，不受外部白名单约束。

非编辑器 Game 只把容器中的 `Plugins/FeatureMods/` 内容视为随项目发布的内置模组；同一路径下的物理松散文件按玩家模组处理，仍需首次确认并受玩家策略约束。这是防止外部文件借用内置路径获得信任的安全规则。使用 loose staging 测试时，如果没有玩家批准处理器，模组会被拒绝加载；验证内置交付应使用实际的 pak 或 IoStore 容器。

官方包使用发布方自己的 Ed25519 密钥；assemble 只接收公钥：

```text
UnrealEditor-Cmd <Project.uproject> -run=ModKitAssemble -Source=<prepared-mod-directory> -Output=<new-directory> -Signer=<trusted-key-id> -PublicKey=<base64-public-key> -unattended -nocrashreports -NoMessaging
```

输出 `<ID>.manifest.json` 是待签草稿，`signature.value` 初始为无效占位值；`<ID>.canonical` 是精确待签字节。外部工具用发布方私钥对 canonical 做 Ed25519 签名，将64字节签名以 Base64 写入 `signature.value`。不要重新编码 canonical，不要修改签名覆盖的文件或字段。可用 OpenSSL 的 `pkeyutl -sign -rawin -inkey <publisher-private-key> -in <ID>.canonical -out <signature.bin>`；私钥不放入 Mod 或交付目录，不使用仓库夹具测试种子。

将 Key ID 加入消费工程的 PatchVerifyKeyId/PatchAcceptedKeyIds，并配置对应 PatchPublicKeyBase64。产品离线验证入口为：

```text
UnrealEditor-Cmd <Project.uproject> -run=ModKitPatchVerify -Kind=package -Manifest=<ID>.manifest.json -PublicKey=<base64-public-key> -MinimumVersion=<minimum-version> -unattended -nocrashreports -NoMessaging
```

该入口验证 package 签名、完整文件集/哈希、引擎版本与可选最低版本，不提交运行时地板。缺省 Kind 仍是 hotpatch，原有预检/落地语义不变。离线通过不替代实际加载与运行验证。

只把 `<ID>/` 和旁置 `<ID>.manifest.json` 部署到外部根；`.canonical` 留在作者端。首次安装官方包时，把已验证的签名清单 provision 到 `Saved/FeatureModVersions/<ID>.json`；后续更新保留现有地板。地板只适用于签名官方包，玩家免签包不需要地板。更新以完整的新目录和签名清单一起交付，装载前仍会验证；签名损坏、缺文件、降级都会拒绝。
