# SoloASCoreExtensions

SoloASCoreExtensions contains optional higher-level features for the SoloAS plugin stack. It depends directly on the established Unreal Engine plugin identity `Angelscript` supplied by the AngelscriptRuntime repository.

This repository is currently private. Its root is the Unreal Engine plugin root and directly contains `SoloASCoreExtensions.uplugin`. Do not wrap it in another `Plugins` directory.

SoloASCoreExtensions consumes only public runtime APIs and has no dependency on integration control-plane code.

## Script lambdas

`BindLambda` and `AddLambda` support typed value captures. Delegate `[this]` and reference captures are rejected; synchronous algorithm lambdas have separate capture rules below. The feature is enabled by default. Empty, reference, initializer and nested captures are unsupported. Keep a binding as a statement, assign its result, or return its result from a method.

```angelscript
int Count = 3;
FScriptLambdaHandle Handle = Changed.AddLambda([int Count](float32 Value) {
    Print("Count: " + Count + ", value: " + Value);
});
Changed.Remove(Handle);
```

Typed captures keep their binding-time values, using the type's normal copy and destruction rules. Captured UObject values are strong GC references. Only captured values, arguments and global names are available to a delegate lambda body. To call an owner object, capture a typed variable containing that object by value; this keeps it alive until the binding is released.

Each script class that declares value-capturing lambdas gets one hidden capture-store property. A call site uses function-name slots on its owner. Binding into an available owner slot creates no target UObject instance. Until detached identities are reclaimed at GC, later bindings can temporarily allocate fallback target objects. A value-capturing site has one UScriptStruct capture schema; each configured slot has a hidden UFunction. These reflection metadata objects have no per-site UClass or CDO.

```ini
[SoloASCoreExtensions]
LambdaSlotsPerSite=2
```

The range is 1–32. Recompile scripts after changing it, and generate precompiled caches with the same setting in a fresh producer process. `1` minimizes generated function metadata. The default `2` lets one owner bind a source location to two different delegates without creating another target object. Further bindings share objects of one native fallback class, up to two occupied slots per object. Raising the setting reduces target instances for large binding sets and increases generated function metadata. There is one fallback UClass and one CDO for the whole feature. Each binding keeps its own values.

Repeated adds at one source location to the same multicast share a capture list. The list runs in addition order. A Broadcast takes a snapshot: new items wait for the next Broadcast, and removed items are skipped. Clear or RemoveAll during a callback stops the remaining group items. Multicast callbacks have no guaranteed execution order relative to other callbacks, consistent with UE.

### Removed script class reclamation

The editor feature `soloas.removed-script-class-reclamation/v1` releases the core root of a successfully removed script class when it has no live instances. Classes with live instances retain the core root. This feature is enabled by default; set `[SoloASCoreExtensions] bEnableRemovedScriptClassReclamation=false` in the project's `Config/DefaultEngine.ini` and restart to keep the core behavior.

### Container algorithms

Container lambdas run synchronously. They generate script functions with ordinary
parameters; no delegate binding, UObject target, slot, pool or handle is created.
The element type comes from the lambda's declared parameter types. Loops and
comparisons stay in the VM and can compile together with the script under AOT.

```angelscript
TArray<int> Values;
Values.Add(3);
Values.Add(1);
int Direction = -1;
Values.Sort([int Direction](int A, int B) {
    return Direction * A < Direction * B;
});
int Removed = 0;
Values.RemoveAll([int& Removed](int Value) {
    if (Value > 1) { ++Removed; return true; }
    return false;
});
```

`TArray` supports `Sort`, `StableSort`, `FindByPredicate`, `IndexOfByPredicate`,
`ContainsByPredicate`, `RemoveAll` and `FilterByPredicate`. Predicates return bool;
sort comparers receive two elements and return whether the first comes before
the second. `StableSort` preserves order among equal elements; `Sort` makes no
such guarantee. The existing `Sort(bool bDescendingOrder)` overload remains
available. `RemoveAll` returns the removed count and retains survivor order.
`IndexOfByPredicate` returns -1 for no match. In AS, use
`bool Found = Values.FindByPredicate([](int Value) { return Value == 3; }, OutValue)`;
this copies the matching element into the output instead of returning the nullable
pointer used by UE C++. Use the index API when you need to modify an array element.

`TSet.FilterByPredicate` receives one element. `TMap.FilterByPredicate` receives
separate typed key and value parameters, such as
`Map.FilterByPredicate([](int Key, const FString& Value) { return Key > 0; })`.
Filtering returns a new container. Maps also support `KeySort` and `ValueSort`,
whose comparers receive two keys or two values. Sorting rebuilds the map in the
new iteration order and invalidates iterators and references, matching UE's
invalidation rule. Keep an explicit `TMap<K, V>` declaration for map sorting and
an explicit `TSet<T>` declaration for set filtering so the source rewriter can
resolve the container type. Existing map/set bindings expose none of these names.

Returned containers, inherited fields and C++ parent fields may lack a visible
container declaration during source rewriting. If the rewriter cannot infer the
container type, it reports the source file and line with a local-reference
workaround. This also applies to single-element array/set filtering, where the
predicate alone cannot distinguish the two container types. For example:

```angelscript
TMap<FName, int>& M = Obj.Scores;
M.KeySort([](FName A, FName B) -> bool {
    return A.ToString() < B.ToString();
});
```

No native callback fallback is provided for these unresolved source expressions.

Typed value captures take one ordinary value snapshot at the algorithm call.
Large values are read through const references to that snapshot during each
comparison; changing the original through another alias does not change it.
Explicit reference captures continue to observe the caller's value.

Algorithm lambdas accept empty captures, typed value captures, and `[this]` in
UObject instance methods. `[T& Name]` additionally passes a local by reference for
the duration of the synchronous call. Untyped `[&]`, capture initializers and nested
lambdas are unsupported. Global functions and struct methods support explicit
captures through global hidden functions; `[this]` is an error there. Without
`[this]`, a lambda body cannot implicitly access the UObject owner's members.

Call-site line numbers remain stable after rewriting. Diagnostics inside extracted
lambda bodies currently refer to generated code, as with delegate lambdas.

### Removing one lambda

Binding returns a lightweight `FScriptLambdaHandle`; discarding it keeps the binding active. Use `Event.Remove(Handle)` or `Delegate.Unbind(Handle)` to remove exactly that lambda. Removing a middle list item leaves the other items active. A stale handle, a handle for a replaced binding or a handle used on another delegate has no effect and emits a warning once. A handle is a value, so copying or discarding it does not unbind anything.

### Lifetime and reuse

Unbind, Clear, rebind, Remove and RemoveAll release captured values when no binding still uses them. Unbind, Remove and Clear detach callbacks immediately. Captured values, detached identities and fallback targets are reclaimed at the next pre-GC reconciliation, after checking for reflected copies. This also applies to C++ and Blueprint changes. A completed one-shot timer and a cleared looping timer release their captures when no other binding remains. Owner destruction and successful hot reload also clear old captures. Running callbacks retain their snapshot through self-unbind and explicit garbage collection. Captured script structs hold a core module-generation lease; each invocation clones its own hold before copying values. Soft reload can publish new generations while old snapshots remain alive. Captures are destroyed before releasing their leases, including during the core closing callback. No execution reader is kept for the binding lifetime. Crossing a complete Runtime module unload with external C++ captures remains unsupported.

Released fallback targets return to a shared pool only after their registered delegates and timers are confirmed detached and no callback is executing. Captures are cleared first, including strong UObject references. The pool uses the transient package, stays independent of worlds, shrinks after idle GC cycles and is flushed on hot reload and shutdown. Defaults are:

| CVar | Default | Meaning |
| --- | ---: | --- |
| `soloas.LambdaPool.MaxIdle` | 128 | Maximum idle target objects |
| `soloas.LambdaPool.IdleGCs` | 3 | Minimum idle GC cycles before shrink |
| `soloas.LambdaPool.PeakGCs` | 4 | Recent peak-use window in GC cycles |

The remaining tracking limit is unreflected native or local/stack delegate storage. Its address cannot safely be revisited, so its identity is retained for owner lifetime and is never reused. Local timer delegates passed to `System::SetTimer` are registered by handle and recycle normally. Ordinary script event bindings recycle automatically. When C++ intentionally saves a delegate in unreflected storage, mark it before copying:

```cpp
SoloASBridge::RetainLambdaForNativeStorage(Delegate);
SavedDelegate = Delegate;
```

Reflected property-to-property copies are found automatically. Native `RemoveAll(Owner)` removes direct owner bindings. Script `Event.RemoveAll(this)` also removes fallback bindings created by that owner; native C++/Blueprint calls keep UE's actual bound-object identity rules for fallback targets. Create, bind, execute and release lambdas on the game thread. No `UObjectPart` or `AddPart` is required.

### Known limits

Binding to another object's reflected delegate can require an expensive first-time storage search. Later binds to that verified destination use a cache until GC or hot reload. Large UI lists can still pay for one search per destination on the first pass.

Clearing the original destination during a callback cancels its executing group snapshot. Delegate copies made during that callback keep the group alive until pre-GC reconciliation; the executing callback cannot identify which copied delegate dispatched it.

After copying a multicast event into another variable, adding a lambda to the original event from the same source location appends to their shared callback group. Both copies will run the new callback. To keep additions independent, add them at different source locations, or use `Remove(Handle)` to remove an individual item.

Set `[SoloASCoreExtensions] bEnableExplicitCaptureLambda=false` in the project's `DefaultEngine.ini` to disable the feature, then restart and regenerate binding/precompiled caches.

Other capabilities register with the plugin without a separate feature switch. The editor Blueprint subclass relink CVar `soloas.EditorBlueprintSubclassRelink` defaults to `1` and can be set to `0`. Blueprint mappings and dependency lists still select the assets to expose; enabling a capability does not grant external/player FeatureMods permission to load or bypass ModKit allowlists or signature checks.

## Blueprint struct binding limitations

The Blueprint struct candidate uses `BlueprintStructBindings` in the `[SoloASCoreExtensions]` section of the project `Config/DefaultEngine.ini`. Register every nested user-defined struct with its own logical alias as well. Asset paths stay in configuration; Runtime settings and `BlueprintStructDependencies` are not used. Bindings and native value operations remain registered for the VM lifetime. Configuration changes require a fresh process and matching regenerated script caches.

Configure the aliases before starting the editor. Asset paths are strings and require double quotes:

```ini
[SoloASCoreExtensions]
+BlueprintStructBindings=(Name=FBlueprintStructEntry,Path="/Game/SoloASTests/ST_BlueprintStructEntry.ST_BlueprintStructEntry")
```

Use your own compiled struct asset and a unique script alias. The struct configuration is independent of the interface and lambda switches.

Registering an additional BlueprintStructs value type after VM startup changes the reflected schema. An interface reload that only edits method bodies can then require a FullReload or restart instead of a soft reload. This is expected behavior; configure value types before starting the editor.

After editing a UDS already used by scripts, restart the editor before resuming scripts. A script FullReload does not rebuild these application value types. The next source preparation rejects a changed schema, but existing VM field accesses and native value operations do not yet have a stale-layout guard. This diagnostic does not protect existing values after an in-process asset edit. Default-value edits, layout migration and preservation of old overridden values are not qualified behavior.

## Blueprint parent classes

Blueprint inheritance and script interfaces are enabled by default for ordinary project scripts and FeatureMods when this plugin is enabled. Configure Blueprint mappings in the project `Config/DefaultEngine.ini`:

```ini
[SoloASCoreExtensions]
+BlueprintAliases=(Name="BP_Enemy",Path="/Game/BlueprintImport/BP_Enemy.BP_Enemy")
```

Restart the editor after changing the feature switch or Blueprint mappings, and regenerate precompiled script caches for Game/cook. Keep the configuration identical across consumers. Script code uses the logical name without an asset path:

```angelscript
import blueprint BP_Enemy;

class UEnemy : BP_Enemy
{
    UFUNCTION(BlueprintOverride)
    void OnBlueprintDeath() {}
}
```

`bEnableScriptInterfaces` defaults to true in code. To disable it, set `[SoloASCoreExtensions] bEnableScriptInterfaces=false` in the project `Config/DefaultEngine.ini`. The project setting takes priority over the code default. The old `[SoloASExtensions]` section is accepted as a read-only alias for `bEnableScriptInterfaces` and `BlueprintAliases` when the corresponding key is absent from `[SoloASCoreExtensions]`; no configuration is rewritten. A configured Blueprint parent is rejected with an extension-not-enabled diagnostic when the switch is off. Legacy `import blueprint` and `CallBlueprint` are provided by SoloASCoreExtensions independently of the script-interface switch. Legacy imports share the project and FeatureMod mapping resolver, with unique asset-name lookup retained when no mapping exists. Blueprints derived from script classes do not require this switch. The same switch enables the editor interface declaration and typed-call slice described below.

No host override or FeatureMod source directory is required to enable this capability. The dependency key is now read from the INI above; it is no longer exposed in the Runtime settings UI. Disabled inheritance reports the required switch. Enabling or disabling the extension changes the binding/cache identity, so regenerate binding and precompiled caches with the same configuration used by Game/cook.

FeatureMod-scoped imports and legacy `BlueprintDependencies` parent names now resolve through FeatureMod.json as described below.



## Script interfaces

With the switch enabled, editor scripts can declare reflected interfaces and implement them through the existing `BlueprintOverride` specifier:

```angelscript
interface IDamageSource
{
    int ComputeDamage(int Value);
    void Notify(int Value) {}
}

class UReceiver : UObject, IDamageSource
{
    UFUNCTION(BlueprintOverride)
    int ComputeDamage(int Value) { return Value + 17; }
}

void ApplyDamage(UObject Object)
{
    IDamageSource Target = Cast<IDamageSource>(Object);
    if (Target != nullptr)
        Target.Notify(Target.ComputeDamage(5));
}
```

Use `interface IChild : IParent {}` for interface inheritance. The first class parent is the unique UObject superclass; subsequent names are reflected interfaces. Native UINTERFACE references retain their existing binding names, such as `UInterfaceSupportContract`. Cast checks `ImplementsInterface`; a non-implementer returns null and a null interface call raises a script exception. Both script and Blueprint implementations can receive typed calls and Blueprint interface messages.

Declarations accept registered reflected types, including `int`, `float`, `bool`, `FString`, `UObject`, and `FVector`. Struct inputs can use `const FVector&`; a mutable `FVector&` copies its updated value back to the caller. The configured AS float precision is preserved. `BlueprintEvent`, `BlueprintCallable`, and `BlueprintPure` are accepted optional modifiers; unsupported types and modifiers are rejected.

Default bodies are supplied to script implementers and to unimplemented Blueprint interface graphs during Blueprint compilation. Authored graphs and events take precedence. Interface values use `FScriptInterface` storage, so script `UPROPERTY` fields and reflected function arguments/results preserve both the UObject reference and UE interface identity. A field keeps its target alive until cleared; a null value still raises an exception when called.

Default method bodies support normal SoftReload: script and Blueprint default calls use the same updated function while implementer class identity stays stable. A saved Blueprint implementer can reopen in a new editor session after startup recreates its AS interface.

Signature changes, interface-parent/implementation-set changes, BPI version changes, serialized interface object fields across sessions, and cook/precompiled consumption remain pending. Declaration changes are currently rejected instead of rebinding a different signature to existing code.

## Blueprint interface imports and mappings

Use a logical name with no asset path in scripts:

```ini
[SoloASCoreExtensions]
+BlueprintAliases=(Name="BPI_Interact",Path="/Game/Interfaces/BPI_Interact.BPI_Interact")
```

```angelscript
import blueprint BPI_Interact;

class UReceiver : UObject, BPI_Interact
{
    UFUNCTION(BlueprintOverride)
    void Interact(UObject Instigator) {}
}
```

The imported asset determines whether the name denotes a Blueprint class or interface. An interface belongs after the UObject parent; using a class in that position is rejected. BPI implementations use the real UE interface identity, and typed calls and Blueprint interface messages share the same target implementation. Unimplemented Blueprint void events are no-ops.

A FeatureMod places its logical mappings in the `BlueprintBindings` object in FeatureMod.json. Its own mapping takes priority, then mappings from declared dependency FeatureMods, then project aliases. Two dependency FeatureMods exporting the same alias are ambiguous and rejected. Project `BlueprintBindings` remains a fallback for the new project key. Missing mappings fail explicitly. Assets are loaded when their import is processed; unrelated new aliases are not preloaded.

Legacy FeatureMod `BlueprintDependencies` parent names use the same scoped mapping resolver when this extension is enabled. The dependency hierarchy remains FeatureMod -> ordinary GFP/trunk; this mapping feature does not permit trunk or GFP code to depend on FeatureMods. Complete binding of members on newly loaded Blueprint parent classes and migration of the legacy CallBlueprint/import-proxy implementation remain in the CallBlueprint stage. Existing project BlueprintDependencies keep their established preloading behavior during migration.

The provider identity is now `soloas.script-interfaces/v3`; rebuild binding and precompiled caches when upgrading from the earlier object-view representation.

## Public-core integration status

On the official core without the legacy extension contracts, Blueprint quick calls,
container algorithm lambdas, and native event super calls use the public bind and
preprocessor APIs. They are enabled by default. Changes to their settings require
an editor or game restart and regeneration of precompiled script caches.

```ini
[SoloASCoreExtensions]
bEnableBlueprintQuickCalls=true
bEnableExplicitCaptureLambda=true
bEnableNativeEventSuper=true
bEnableToolsPackagingProvider=true
bEnableExplicitReloadRequest=true
```

The lambda setting controls container algorithms, dynamic delegate captures, and
native delegate capture dispatch through `AngelscriptOptionalProviders.h`.
A native binding plugin must register the provider; CE does not depend on MBC or
own native delegate storage. Unsupported native bindings raise a script diagnostic.
Script interface declarations,
Blueprint UInterface identity, Blueprint structs, and Blueprint imports/inheritance
still require additional integration on this core.

The editor explicit full-reload transaction is enabled by default. Disable it with
`bEnableExplicitReloadRequest=false` and restart the editor. Ordinary requests pass
an empty fingerprint to `CommitExplicitFullReload`; a busy runtime is retried until
the reservation expires or is cancelled. A committed soft reload is reported as a
failure to perform a full reload, with the committed VM state recorded in the receipt.
Requests with an approved fingerprint are rejected before submission until the core
can verify the loaded content before committing it. Inspect the receipt for the reason.

The editor Tools endpoint `SoloAS.PackagingProvider.v1.core_extensions` provides
`describe`, `core_extensions.precheck`, and `core_extensions.validate_caches`.
During successful `-as-generate-precompiled-data` execution, the runtime module
writes `Script/CoreExtensions.packaging.json` after the initial compilation. It
records the CE source identity, frozen feature settings, and digests of
`Binds.Cache`, `Binds.Cache.Headers`, and `PrecompiledScript.Cache`. Keep these
files together. Validation rejects a missing manifest, a different producer
identity, missing files, or changed bytes. Precheck and this validation do not
certify JIT pairing, package containers, or the core's runtime cache loader.

Source callbacks and the synchronous Tools endpoint can be withdrawn. VM types,
functions, aliases, and their native userdata remain valid until VM destruction.
The bundled VM's config-group removal currently returns success without removing
registrations. Same-VM withdrawal depends on the planned generic VM registration
scope (`vmscope`), which is implemented serially by the core owner. Until that
scope is available, withdrawal takes effect after restart or reconstruction of the
next VM generation. The runtime extension module therefore remains loaded for the
current VM's lifetime.
