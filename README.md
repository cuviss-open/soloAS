English | [简体中文](README.zh-CN.md)

# soloAS

soloAS is an Unreal scripting system built for design. It adds AngelScript scripting as plugins for the stock engine, so you get an excellent development experience without modifying engine code.

## Documentation

[Project introduction](https://soloas.cuviss.com/docs/en/overview.html)

## Download

Current adaptation: **UE 5.8.4**.

[Release v0.1.2](https://github.com/cuviss-open/soloAS/releases): Win64 precompiled package (source and binaries), symbols package (PDB debugging symbols), and source package (requires compilation).

## Included plugins

| Folder in the source package | Plugin identity | Purpose / activation |
| --- | --- | --- |
| `AngelscriptRuntime/` | `Angelscript` | Core runtime and editor integration; enabled by default |
| `SoloASBridge/` | `SoloASBridge` | Stock engine adaptation; enabled by the runtime dependency |
| `AngelscriptManualBindPlugins/AngelscriptGAS/` | `AngelscriptGAS` | Optional Gameplay Ability System bindings; off by default |
| `AngelscriptManualBindPlugins/AngelscriptEnhancedInput/` | `AngelscriptEnhancedInput` | Optional Enhanced Input bindings; off by default |

## License

| Component | License |
| --- | --- |
| AngelscriptRuntime (including the AngelScript library) | MIT; the AngelScript library is licensed under zlib |
| AngelscriptGAS | Apache-2.0 or MIT, at your option |
| AngelscriptEnhancedInput | MIT |
| All other content (including SoloASBridge) | MPL-2.0 |

- See [NOTICE](NOTICE), each plugin's license files and retained source notices.

Note: For plugins inherited from the UE Haze fork, our modifications to them are licensed under that plugin’s original license. Everything else is MPL-2.0.

Credits and showcase (optional): please credit soloAS and share your public project; showcase entries can be changed or removed on request.

Contact: [GitHub Issues](https://github.com/cuviss-open/soloAS/issues) · [contact@cuviss.com](mailto:contact@cuviss.com).

[UPSTREAM.md](UPSTREAM.md) · [CONTRIBUTING.md](CONTRIBUTING.md) · [LICENSE](LICENSE)
