# SoloASBridge

SoloASBridge is the compatibility bridge between AngelScript-compatible consumers and an unmodified Unreal Engine. It replaces engine-source modifications with plugin-owned public APIs, build-time adapters, pre-UHT tooling, external integration points, and narrow runtime bridges.

This repository is currently private. Its root is the Unreal Engine plugin root and directly contains `SoloASBridge.uplugin`. Do not wrap it in another `Plugins` directory.

SoloASBridge exposes only public APIs and has no dependency on higher-level scripting plugins or integration control-plane code.
