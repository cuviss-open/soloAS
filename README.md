English | [简体中文](README.zh-CN.md)

# soloAS

## Documentation

- [Home](https://soloas.cuviss.com/)
- [Installation / quick start](https://soloas.cuviss.com/docs/en/requirements.html)
- [Script introduction](https://soloas.cuviss.com/docs/en/learn/index.html)
- [GAS](https://soloas.cuviss.com/docs/en/manual-binding-packs.html#gas-write-abilities-and-attributes-in-script)
- [Enhanced Input](https://soloas.cuviss.com/docs/en/manual-binding-packs.html#enhanced-input-respond-to-input-in-script)
- [API reference](https://soloas.cuviss.com/docs/en/learn/binding-manual-reference.html)
- [FAQ / differences](https://soloas.cuviss.com/docs/en/core.html)
- [Roadmap](https://soloas.cuviss.com/#roadmap)
- [UPSTREAM.md](UPSTREAM.md) · [CONTRIBUTING.md](CONTRIBUTING.md) · [LICENSE](LICENSE) · [RELEASE_NOTES.md](RELEASE_NOTES.md)

## About

- AngelScript scripting for unmodified Unreal Engine, delivered as project plugins; compatible with common workflows from the Hazelight fork.
- Version **0.1.0+ue5.8.3** · tag **v0.1.0** · [Release downloads](https://github.com/cuviss-open/soloAS/releases/tag/v0.1.0).

## Upstream snapshot

- Original Epic UE and Hazelight fork snapshots, commits and component source paths: [UPSTREAM.md](UPSTREAM.md), [upstream.json](upstream.json).
- Hazelight usage documentation: [angelscript.hazelight.se](https://angelscript.hazelight.se/).


## Included plugins

| Folder in the source package | Plugin identity | Purpose / activation |
| --- | --- | --- |
| `AngelscriptRuntime/` | `Angelscript` | Core runtime and editor integration; enabled by default |
| `SoloASBridge/` | `SoloASBridge` | Stock engine adaptation; enabled by the runtime dependency |
| `AngelscriptManualBindPlugins/AngelscriptGAS/` | `AngelscriptGAS` | Optional Gameplay Ability System bindings; off by default |
| `AngelscriptManualBindPlugins/AngelscriptEnhancedInput/` | `AngelscriptEnhancedInput` | Optional Enhanced Input bindings; off by default |

## Requirements

- **Unreal Engine 5.8.3**: the Launcher build is recommended.
- A source build of the same tag is theoretically equivalent, but not guaranteed; modified engines are unsupported.
- **Windows 11 x64** is the primary platform. Windows 10 is expected to work but has not been verified.
- **macOS 15+** has limited compile verification, with no behavior guarantee.
- Win64 precompiled package: Blueprint projects can use it directly in the editor. Windows 10 needs 7-Zip to extract it; Windows 11 can extract it directly.
- Packaging Win64 projects, including Blueprint-only projects, requires the UE-supported C++ toolchain: MSVC 14.44 and Windows SDK 10.0.22621.0. Installed plugin modules use the supplied precompiled artifacts.
- Source package: a C++ Unreal project and a configured Unreal C++ toolchain; Visual Studio on Windows. Add a C++ target first for a Blueprint-only project.

## Quick start

1. Choose a package from [Releases](https://github.com/cuviss-open/soloAS/releases/tag/v0.1.0) and verify it against `SHA256SUMS`:
   - **Win64 precompiled**: `soloAS-0.1.0-ue5.8.3-win64.7z` includes source and Win64 binaries. Extract into `<Project>/Plugins/`; Blueprint projects can use it directly in the editor. Windows 10 needs 7-Zip; Windows 11 can extract it directly.
   - **Source**: `soloAS-0.1.0-ue5.8.3-source.zip` requires a C++ project and the matching toolchain.
   - Optional symbols: download `soloAS-0.1.0-ue5.8.3-win64-symbols.7z` for debugging or crash stack analysis; extract over the same installation. It contains PDB files only.
2. Close the editor. For the source package, extract into `<Project>/Plugins/soloAS/`, preserving its directory layout. Keep `AngelscriptRuntime/` as named: Unreal identifies the plugin from `Angelscript.uplugin`. Install each plugin only once.
3. For the source package, generate Visual Studio project files from the `.uproject` and build `<ProjectName>Editor` for **Win64 / Development**. For the precompiled package, open the project directly.
4. The core runtime enables `SoloASBridge` automatically. Enable `AngelscriptGAS` or `AngelscriptEnhancedInput` in the Plugins window only when needed; restart when prompted.
5. Save the following as `<Project>/Script/FirstScriptActor.as`:

   ```angelscript
   class AFirstScriptActor : AActor
   {
       bool bHelloWorldShown = false;

       UFUNCTION(BlueprintOverride)
       void BeginPlay()
       {
           Print("Hello from soloAS!");
           bHelloWorldShown = true;
       }
   }
   ```

6. Place the script actor in a level or spawn it from game logic, then play. `BeginPlay` prints **Hello from soloAS!** for five seconds and records it in the Output Log.

- Ordinary `.as` edits compile and reload without restarting the editor.
- After changing native plugin code or installing/replacing plugins, close the editor, regenerate project files and rebuild the Editor target.
- [Detailed setup](https://soloas.cuviss.com/docs/en/requirements.html)

## VS Code

- Install Hazelight's **Unreal Angelscript** extension; see the [AngelScript documentation](https://angelscript.hazelight.se/).
- Open the project's `Script/` folder as the workspace.
- Completion, diagnostics, navigation, breakpoints, variable inspection and continue use the runtime DebugServer.
- Start the editor without `-NoMessaging` or `-NoASDebugServer`, which disable that connection.

## Migration from the Haze fork

- Use unmodified UE 5.8.3 and replace the fork's AngelScript plugin with the included plugins; remove duplicate installations.
- Keep the project's scripts under `Script/` and rebuild its C++ Editor target.
- For assets with an empty saved engine version, use **SoloAS → Resave assets with empty engine version**, then review the source-control changes.
- CoreExtensions features—script interfaces, capturing lambdas and Blueprint parent import—are outside this release.
- [Migration guide](https://soloas.cuviss.com/docs/en/migration.html)

## Script delivery

- **Source**: package `.as` files; the runtime compiles them at startup. Pak files do not keep source secret.
- **Bytecode**: generate matching precompiled data with the Shipping Game executable, preserve the three matching files `Binds.Cache`, `Binds.Cache.Headers` and `PrecompiledScript.Cache` from the same run, and package without the main `.as` source. Editor-generated caches cannot substitute for Game caches.
- **Generated C++**: compile supported generated script functions into the game C++ module and retain the matching three caches; regenerate after changing scripts, plugins, engine or other native code.
- Bytecode is not encryption or reverse-engineering protection.
- [Script delivery commands and package checks](https://soloas.cuviss.com/docs/en/packaging.html)

## Differences and limits

- FullReload takes about **1.6–1.8×** the Haze fork's time, a known adaptation cost; ordinary script edits need no editor restart.
- Compatibility covers the core workflow and common fork usage; it does not imply every fork tool or API is included.
- Windows is the primary validation platform; macOS verification is limited to compilation.
- [Differences from Haze](https://soloas.cuviss.com/docs/en/core.html)

## Roadmap

- Planned for future releases: core extensions, automatic binding, FeatureMod support, packaging tools, manual binding core and additional binding packs.
- Scope, order and release timing may change; these are intended directions.
- [Roadmap](https://soloas.cuviss.com/#roadmap)

## License

| Component | License |
| --- | --- |
| SoloASBridge | MPL-2.0 |
| AngelscriptRuntime plugin source, including soloAS changes | MIT |
| AngelScript library, including modifications | zlib |
| AngelscriptGAS, including soloAS changes | Apache-2.0 or MIT, at your option |
| AngelscriptEnhancedInput, including soloAS changes | MIT |

- See [NOTICE](NOTICE), each plugin's license files and retained source notices.
- When distributing a compiled product containing MPL-covered code, provide recipients the corresponding source access required by MPL-2.0.

## Credits and showcase

- Both conventions below are **optional**, not additional license conditions.
- Please credit **soloAS** with a link to <https://soloas.cuviss.com/> in your credits, store page, README or website.
- Public projects that state they use soloAS may be listed as showcases, using only the project name, a short description, public screenshots or links.
- Showcase entries can be removed on request; submissions are welcome.

## Contact

- Bugs and feedback: [GitHub Issues](https://github.com/cuviss-open/soloAS/issues).
- Email: [contact@cuviss.com](mailto:contact@cuviss.com).
