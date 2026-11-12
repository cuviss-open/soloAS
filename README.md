English | [简体中文](README.zh-CN.md)

# soloAS

soloAS is an Unreal scripting system built for design. It adds AngelScript scripting as plugins for the stock engine, so you get an excellent development experience without modifying engine code.

## Documentation

[Project introduction](https://soloas.cuviss.com/docs/en/overview.html)

[Discord community](https://discord.gg/D8W94mvMMU)

## Download

See [Environment setup](https://soloas.cuviss.com/docs/en/requirements.html) and the [Changelog](https://soloas.cuviss.com/docs/en/changelog.html) for supported Unreal Engine versions.

[Latest release](https://github.com/cuviss-open/soloAS/releases/latest): Win64 precompiled package (source and binaries), symbols package (PDB debugging symbols), and source package (requires compilation).

## Included plugins

<!-- soloas-plugins:start -->
| Folder in the source package | Plugin identity | Purpose / activation |
| --- | --- | --- |
| `AngelscriptRuntime/` | `Angelscript` | Core runtime and editor integration; enabled by default |
| `SoloASBridge/` | `SoloASBridge` | Stock engine adaptation; enabled by dependency |
| `AngelscriptManualBindPlugins/AngelscriptGAS/` | `AngelscriptGAS` | Optional Gameplay Ability System bindings; off by default |
| `AngelscriptManualBindPlugins/AngelscriptEnhancedInput/` | `AngelscriptEnhancedInput` | Optional Enhanced Input bindings; off by default |
| `AngelscriptModKit/` | `AngelscriptModKit` | FeatureMod loading and script updates; enabled by default |
| `SoloASCoreExtensions/` | `SoloASCoreExtensions` | Optional high-level scripting extensions; enabled by default |
| `SoloASDeveloperTools/` | `SoloASDeveloperTools` | Editor and project development tools; enabled by default |
<!-- soloas-plugins:end -->

## License

| Component | License |
| --- | --- |
| AngelscriptRuntime (including the AngelScript library) | MIT; the AngelScript library is licensed under zlib |
| AngelscriptGAS | Apache-2.0 or MIT, at your option |
| AngelscriptEnhancedInput | MIT |
| All other content (including SoloASBridge) | MPL-2.0 |

- See [NOTICE](NOTICE), each plugin's license files and retained source notices.

Note: For plugins inherited from the UE-AS fork (UnrealEngine-Angelscript, maintained by Hazelight), our modifications are licensed under that plugin’s original license. Everything else is MPL-2.0.

Credits and showcase (optional): please credit soloAS and share your public project; showcase entries can be changed or removed on request.

Contact: [GitHub Issues](https://github.com/cuviss-open/soloAS/issues) · [contact@cuviss.com](mailto:contact@cuviss.com).

[UPSTREAM.md](UPSTREAM.md) · [CONTRIBUTING.md](CONTRIBUTING.md) · [LICENSE](LICENSE)
