# Shared delivery contract v1

## Consumers and trust

`Shared/Delivery/ModKitPatchManifest.h` is the shared signed-delivery model for FeatureMods and HotPatch. `Shared/Crypto/ModKitCacheCipher.h` owns `FModKitSha256::HashHex(Data, NumBytes)` (lowercase SHA-256); ScriptCache keeps its existing cipher/keystore API. Cache encryption or a cache hash never substitutes for package source verification.

Consumers pass startup-frozen public keys and trusted signer IDs. Shared does not read settings, classify player/official/built-in mods, prompt users, or enable unsigned content. Unsigned player policy belongs to FeatureMods; unsigned input never creates a signed floor.

## Manifest and signatures

- Existing `ModKitHotPatchManifest/v1` wire format and canonical UTF-8/LF bytes remain unchanged. Missing `kind` means `hotpatch` for v1 compatibility; explicit values must be string `hotpatch` or `package`. Package canonical bytes include `kind=package`; hotpatch canonical bytes omit that line. Changing kind invalidates the signature.
- `Parse(Json, Out, Reason)` validates fields, budgets, paths, dot-separated numeric versions, lowercase 64-hex hashes and 64-byte Ed25519 signatures. Hotpatch entries must end in `.as`; package entries may include descriptor, `FeatureMod.json`, scripts and assets. Duplicate paths ignoring case are rejected.
- `Manifest.VerifySignature(ExpectedKind, PublicKeyBase64, TrustedKeyIds, Reason)` verifies kind, trusted signer, public-key fingerprint, canonical payload hash and Ed25519 signature. It does not validate engine, package identity, precheck or file bytes.
- `Manifest.VerifyFiles(FileBytes, Reason)` requires an exact manifest-relative POSIX path set and exact SHA-256 bytes, within per-file and aggregate budgets. Keys match case exactly. Read the physical input once and use the accepted immutable byte snapshot for compilation/copying; do not reopen files after validation.
- HotPatch must require `hotpatch`, `MatchesEngineVersion()`, precheck PASS and its existing baseline/safe-point/rollback conditions before landing/applying. FeatureMods must require `package`, descriptor identity and engine match before accepting sources. ScriptCache must retain source-only leaf filtering and its own runtime/cache identity.

## Signed package floor transaction

`FModKitPatchManifest::VerifyAndCommitPackageFloor(ManifestJson, FileBytes, ExpectedId, ExpectedVersion, PublicKeyBase64, TrustedKeyIds, FloorPath, bAllowDowngrade, Reason)` parses and verifies the package, current engine, descriptor identity and complete byte snapshot. It then locks the absolute normalized floor path across processes, verifies any existing signed package floor with the same trust policy and identity, rejects rollback unless explicitly permitted, and atomically writes the original signed JSON when creating or advancing the floor. A permitted downgrade never lowers the stored floor. Missing state is first installation; malformed/unreadable/untrusted state fails closed. Failed verification never creates or changes a floor. Write/lock failures reject acceptance.

FeatureMods supplies its existing `Saved/FeatureModVersions/<id>.json` path and retains its process-local highest-accepted-version check. It must remove the old missing-floor rejection and duplicated floor read/write transaction, call this shared transaction with the full snapshot, then publish verified source/package maps only on success. Read descriptor bytes from that same snapshot; bytecode cannot replace signed source bytes. No floor is created for unsigned players or built-in mods. HotPatch retains its separate apply-version state and must not call the package transaction.

## Stable result reasons

Success clears `Reason`. Shared signature failures: `kind`, `key-id-untrusted`, `key-missing`, `signature-anchor`, `signature-invalid`. File failures: `package-file-set-mismatch`, `package-file-missing`, `package-byte-limit`, `package-file-hash`. Floor failures: `manifest-invalid`, `identity-mismatch`, `engine-mismatch`, `version-state-lock`, `version-state-invalid`, `version-rollback`, `version-state-write`. Consumers may translate these into their existing logs/UI, preserving failed outcomes.

## Integration ownership

S owns Shared implementation and this contract. F owns the FeatureMods call sites and representative install/update/reload test. H owns HotPatch call sites and precheck/apply policy. K owns ScriptCache call sites and cache/JIT identity. Shared is compiled in the Runtime module in Editor and Game; it introduces no Runtime/Bridge registration or settings.
