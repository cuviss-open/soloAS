# SoloASBridge

SoloASBridge connects AngelScript integrations to an unmodified Unreal Engine. It replaces engine-source changes with public engine APIs, build-time adapters, tooling that runs before Unreal Header Tool (UHT), external integration points and focused runtime bridges. / SoloASBridge 将 AngelScript 集成到未经修改的 Unreal Engine，通过引擎公开接口、构建适配、Unreal Header Tool（UHT）运行前的工具、外部集成接口及专用运行时适配替代引擎源码修改。

This directory is the Unreal Engine plugin root and directly contains `SoloASBridge.uplugin`. Do not add another `Plugins` directory inside it. / 本目录即 Unreal Engine 插件根目录，直接包含 `SoloASBridge.uplugin`，不要在其中再添加一层 `Plugins` 目录。

SoloASBridge exposes public APIs and does not depend on higher-level scripting plugins or private development tools. / SoloASBridge 提供公开接口，不依赖上层脚本插件或私有开发工具。
