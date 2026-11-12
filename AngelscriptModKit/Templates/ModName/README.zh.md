# ModName FeatureMod 模板

将目录放在 `Plugins/FeatureMods/<名称>/` 随项目发布，或放在游戏根 `FeatureMods/<名称>/`
供外部安装与热更新；目录名与 .uplugin 文件名是稳定 ID。
.uplugin 用 `"FeatureMod": true` 标记身份，并保留模板中的 `ExplicitlyLoaded=true`
和 `BuiltInInitialFeatureState=Installed`；业务配置写 FeatureMod.json，按
FeatureMod.schema.json 校验。项目的 API、签名与加载策略写在 Config/DefaultEngine.ini
的 `[/Script/AngelscriptModKit.ModKitSettings]`；编辑器目录与热重载选项在
Config/DefaultFeatureMods.ini。设置修改后重启生效。

创建时将文件名和文件内容中的 `ModName` 替换为稳定 ID；示例类
`AModNameActor` 同步变为 `A<名称>Actor`，让多个模板 Mod 可以同时加载。

玩家 Mod 默认可使用 API、原生基类和事件覆盖。ModKit 默认封锁控制台命令、文件读写、
启动外部程序或 URL、退出、按名查找对象并改属性，以及清单中的编辑器专用 API；存档
读写保持开放，只封 `Gameplay::DeleteGameInSlot`。项目可在 `Config/DefaultEngine.ini`
用 `+DeniedApiPatterns` 追加规则，或用 `-DeniedApiPatterns=*System::QuitGame*` 放开
QuitGame。已分类为 `internal` 的 API 始终拒绝。首次加载前需要用户确认；内置和官方
Mod 不受 API 黑名单限制。

```ini
[/Script/AngelscriptModKit.ModKitSettings]
+DeniedApiPatterns=*MyDangerousApi::Call*
-DeniedApiPatterns=*System::QuitGame*
```

## 依赖层级

树干（主体/普通插件）→ 枝干（普通 GFP）→ 叶子（FeatureMod）；依赖只指向已声明的同层或内层目标。
普通插件和 GFP 均可含 AS；需要独立安装、卸载或热更新时才使用 FeatureMod。

创建、依赖、同名解析、BlueprintBindings 和发布步骤统一见随 SDK 交付的 `Docs/SDK.zh.md`。
开发平台为 Stock UE + SoloAS。

功能模组可交付源码或按 MK14/MK18 生成的配对模块缓存；主体使用半 AOT 不会自动把功能模组变成静态代码。
管理器 Assemble 或 `-run=ModKitAssemble` 可独立组装准备好的目录，不要求 DeveloperTools。
cache-only 交付须先生成缓存并移除源码，assemble 不替代缓存生成或资产 cook。
完整作者命令、GFP 依赖、外部签名与离线验证见 `Docs/SDK-Delivery.zh.md` 的“独立功能模组交付”。
官方包使用外部 Ed25519 工具生成旁置 `<名称>.manifest.json`，列出全部文件及 SHA-256；
FeatureMod.json 中的旧 signature 字段不能替代签名包清单。项目构建时配置可信公钥和 Key ID，
不使用仓库测试夹具中的密钥发布。
