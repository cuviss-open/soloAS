# Third-party components

Microsoft Detours 4.0.1 is used for editor-only Windows hooks on UE 5.7.

- Source: https://github.com/microsoft/Detours/tree/v4.0.1
- Commit: `e4bfd6b03e50de46b47abfbd1e46b384f0c5f833`
- License: MIT; original text in [LICENSE.md](Source/ThirdParty/Detours/LICENSE.md).
- The required upstream source files are unchanged. UBT builds them from source.
- UE 5.8 uses the engine-provided Detours module. Mac uses its existing hook implementation.
