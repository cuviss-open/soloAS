using System;
using System.Collections.Generic;
using System.Linq;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using System.Text.Json;
using EpicGames.Core;
using EpicGames.UHT.Tables;
using EpicGames.UHT.Tokenizer;
using EpicGames.UHT.Types;
using EpicGames.UHT.Utils;
// UE 5.4 represents one module as one package. 
#if SOLOAS_UHT_UE54
using UhtModule = EpicGames.UHT.Types.UhtPackage;
#endif
namespace SoloASBridgeUht;
[UnrealHeaderTool]
public static class SoloASBridgeExporter
{
    private const string OutputBaseName = "SoloASBridgeScriptMetadata";
    private const string OutputExtension = ".soloasbridge.uht.inl";
    private const ushort CppConstTrait = 1 << 0;
    private const ushort CppRefTrait = 1 << 1;
    private const ushort CppEnumAsByteTrait = 1 << 2;
    private const ushort WorldContextTrait = 1 << 4;
    private const ushort ConstTemplateArgTrait = 1 << 5;
    private const ushort ScriptCallableFunctionTrait = 1 << 0;
    private const ushort WorldContextFunctionTrait = 1 << 2;
    private const ushort NoNativeCallerFunctionTrait = 1 << 4;
    [UhtExporter(
        Name = "SoloASBridgeMetadata",
        Description = "Export module-owned script metadata through Core.",
        ModuleName = "SoloASBridge",
#if SOLOAS_UHT_UE54
        Options = UhtExporterOptions.Default,
#else
        Options = UhtExporterOptions.Default | UhtExporterOptions.CompileOutput,
#endif
        CppFilters = new[] { "*.soloasbridge.cpp", "*.soloasbridge.gen.cpp" },
        HeaderFilters = new[] { "*.soloasbridge.types.h" }
    )]
    public static void Export(IUhtExportFactory factory)
    {
        Task? task = factory.CreateTask(ExportMetadata);
        task?.GetAwaiter().GetResult();
    }
    private static void ExportMetadata(IUhtExportFactory factory)
    {
        // BuildPlugin marks its external plugin as EngineRuntime too. Only modules
        // physically installed with the engine belong in Bridge's native baseline.
        DirectoryReference engineDirectory = new(factory.Session.EngineDirectory!);
        DirectoryReference engineSource = DirectoryReference.Combine(engineDirectory, "Source");
        DirectoryReference enginePlugins = DirectoryReference.Combine(engineDirectory, "Plugins");
        List<UhtModule> modules =
#if SOLOAS_UHT_UE54
            factory.Session.Packages.Where(module =>
#else
            factory.Session.Modules.Where(module =>
#endif
        {
            DirectoryReference source = new(module.Module.BaseDirectory);
            return (!source.IsUnderDirectory(engineSource) && !source.IsUnderDirectory(enginePlugins))
                || module.Module.SaveExportedHeaders;
        }).ToList();
        List<UhtHeaderFile> nativeHeaders = factory.Session.HeaderFiles.Where(header =>
#if SOLOAS_UHT_UE54
            !modules.Contains(header.Package)).ToList();
#else
            !modules.Contains(header.Module)).ToList();
#endif
        ExportNativeMetadata(factory, nativeHeaders);
        ExportHeaders(factory, Array.Empty<UhtHeaderFile>(), null);
        Dictionary<UhtModule, List<UhtHeaderFile>> consumerHeaders = new();
        HashSet<string> sourceBuildRequired = NativeSourceBuildSkips(factory);
        string callerPath = Path.GetFullPath(Path.Combine(factory.PluginModule!.BaseDirectory,
            "..", "..", "Config", "NativeCallerModules.json"));
        factory.AddExternalDependency(callerPath);
        using JsonDocument callerDocument = JsonDocument.Parse(File.ReadAllText(callerPath));
        Dictionary<string, string> callerModules = callerDocument.RootElement.GetProperty("modules")
            .EnumerateArray().ToDictionary(row => row.GetProperty("callerModule").GetString()!,
                row => row.GetProperty("module").GetString()!, StringComparer.Ordinal);

                                                                                                     
        foreach (UhtModule consumer in modules)
        {
            if (!callerModules.TryGetValue(consumer.ShortName, out string? nativeModule)) continue;
            if (sourceBuildRequired.Contains(nativeModule)) continue;
            // Consume the actual Build.cs dependency decision, including every early return.
            if (!consumer.Module.PublicDefines.Contains("SOLOAS_NATIVE_CALLER_AVAILABLE=1")) continue;
            consumerHeaders.Add(consumer, nativeHeaders.Where(header =>
#if SOLOAS_UHT_UE54
                header.Package.ShortName == nativeModule).ToList());
#else
                header.Module.ShortName == nativeModule).ToList());
#endif
        }
                                                                                                   
        foreach (UhtModule module in modules.OrderBy(item => item.ShortName, StringComparer.Ordinal))
#if SOLOAS_UHT_UE54
            ExportHeaders(factory, factory.Session.HeaderFiles.Where(header => header.Package == module), module, consumerHeaders.GetValueOrDefault(module), callerModules.GetValueOrDefault(module.ShortName));
#else
            ExportHeaders(factory, module.Headers, module, consumerHeaders.GetValueOrDefault(module), callerModules.GetValueOrDefault(module.ShortName));
#endif
    }
                                                                                                   
    private static HashSet<string> NativeSourceBuildSkips(IUhtExportFactory factory)
    {
        string path = Path.GetFullPath(Path.Combine(factory.PluginModule!.BaseDirectory,
            "..", "..", "Config", "NativeCallerSkips.json"));
        factory.AddExternalDependency(path);
        using JsonDocument version = JsonDocument.Parse(File.ReadAllText(
            Path.Combine(factory.Session.EngineDirectory!, "Build", "Build.version")));
        int minor = version.RootElement.GetProperty("MinorVersion").GetInt32();
        using JsonDocument policies = JsonDocument.Parse(File.ReadAllText(path));
        HashSet<string> skipped = new(StringComparer.Ordinal);
        foreach (JsonElement policy in policies.RootElement.GetProperty("versions").EnumerateArray())
            if (policy.GetProperty("engineMinor").GetInt32() == minor)
                foreach (JsonElement module in policy.GetProperty("skippedModules").EnumerateArray())
                    skipped.Add(module.GetProperty("module").GetString()!);
        return skipped;
    }
                                                                                                 
                                                                                              
    private static void ExportNativeMetadata(IUhtExportFactory factory, IEnumerable<UhtHeaderFile> headers)
    {
        List<FunctionRecord> functions = new();
        List<DefaultArgumentRecord> defaults = new();
        List<NativePropertyTraitRecord> properties = new();
        List<NativeFunctionTraitRecord> traits = new();
        List<UhtFunction> callers = new();
        foreach (UhtHeaderFile header in headers)
            foreach (UhtType child in header.Children)
                CollectFunctions(child, functions, defaults, properties, traits, callers, false, true);
        SortAndRejectDuplicates(functions, "native script function");
        SortAndRejectDuplicates(defaults, "native default argument");
        SortAndRejectDuplicates(properties, "native property trait");
        SortAndRejectDuplicates(traits, "native function trait");
        string json = JsonSerializer.Serialize(new { functions, defaultArguments = defaults,
            propertyTraits = properties, functionTraits = traits.Select(row => new {
                row.Owner, row.Function, row.TraitBits, HasNativeCaller = row.Caller != null }) },
            new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase });
        factory.CommitOutput(factory.MakePath("SoloASBridgeNativeMetadata", ".json"), new StringBuilder(json));
    }
                                                                                            
    private static void ExportHeaders(
        IUhtExportFactory factory, IEnumerable<UhtHeaderFile> headers, UhtModule? module,
        IEnumerable<UhtHeaderFile>? consumedHeaders = null, string? nativeModuleName = null)
    {
        List<FunctionRecord> functions = new();
        List<DefaultArgumentRecord> defaultArguments = new();
        List<NativePropertyTraitRecord> propertyTraits = new();
        List<NativeFunctionTraitRecord> functionTraits = new();
        List<UhtFunction> referenceCallers = new();
        foreach (UhtHeaderFile header in headers.OrderBy(
            item => item.ModuleRelativeFilePath, StringComparer.Ordinal))
        {
            foreach (UhtType child in header.Children)
            {
                CollectFunctions(child, functions, defaultArguments, propertyTraits, functionTraits, referenceCallers, module != null);
            }
        }
                                                                                                              
        if (consumedHeaders != null)
        {
            List<FunctionRecord> consumedFunctions = new();
            List<DefaultArgumentRecord> consumedDefaults = new();
            List<NativePropertyTraitRecord> consumedProperties = new();
            List<NativeFunctionTraitRecord> consumedTraits = new();
            List<UhtFunction> consumedCallers = new();
            foreach (UhtHeaderFile header in consumedHeaders)
                foreach (UhtType child in header.Children)
                    CollectFunctions(child, consumedFunctions, consumedDefaults, consumedProperties,
                        consumedTraits, consumedCallers, false, true);
            referenceCallers.AddRange(consumedCallers);
            // Declaration traits also describe fields and native exec reference inputs.
            // Neither requires an exported typed caller; reference returns remain gated
            // by GetNativeReferenceCaller in the Runtime reflection adapter.
            propertyTraits.AddRange(consumedProperties);
            functionTraits.AddRange(consumedTraits.Where(record => record.Caller != null));
        }
                                                                                                            
        SortAndRejectDuplicates(functions, "script function");
        SortAndRejectDuplicates(defaultArguments, "ScriptCallable default argument");
        SortAndRejectDuplicates(propertyTraits, "native function property trait");
        SortAndRejectDuplicates(functionTraits, "native function trait");
        StringBuilder output = BuildInline(functions, defaultArguments, propertyTraits, functionTraits);
        if (module == null)
        {
            factory.CommitOutput(factory.MakePath(OutputBaseName, ".native-returns.h"),
                BuildNativeReferenceCallers(factory, referenceCallers));
            factory.CommitOutput(factory.MakePath(OutputBaseName, OutputExtension), output);
            return;
        }
        string providerPath = Path.Combine(factory.PluginModule!.BaseDirectory,
            "Public", "Shared", "SoloASBridgeScriptMetadataProvider.h");
        factory.AddExternalDependency(providerPath);
        string providerName = "sb.soloasbridge.types.h";
        factory.CommitOutput(Path.Combine(module.Module.OutputDirectory, providerName),
            new StringBuilder(File.ReadAllText(providerPath)));
        output.Insert(0, $"#include \"{providerName}\"\n" + BuildNativeReferenceCallers(factory, referenceCallers)
            + $"namespace SoloASBridgeGenerated_{module.ShortName} {{\n");
        bool companion = nativeModuleName != null;
        output.Append("static FSoloASBridgeScriptMetadataRegistration Registration(\n    \"")
            .Append(nativeModuleName ?? module.ShortName).Append("\",\n")
            .Append("    MakeArrayView(GeneratedScriptFunctions, GeneratedScriptFunctionCount),\n")
            .Append("    MakeArrayView(GeneratedScriptDefaultArguments, GeneratedScriptDefaultArgumentCount),\n")
            .Append("    MakeArrayView(GeneratedNativeFunctionPropertyTraits, GeneratedNativeFunctionPropertyTraitCount),\n")
            .Append("    MakeArrayView(GeneratedNativeFunctionTraits, GeneratedNativeFunctionTraitCount)")
            .Append(companion ? ", false);\n}\n" : ");\n}\n");
        if (companion)
            output.Append("void ").Append(module.ShortName).Append("_Register() { SoloASBridgeGenerated_")
                .Append(module.ShortName).Append("::Registration.Register(); }\nvoid ")
                .Append(module.ShortName).Append("_Unregister() { SoloASBridgeGenerated_")
                .Append(module.ShortName).Append("::Registration.Unregister(); }\n");
        // Plugin MakePath always redirects to PluginModule, even with a module argument.
        factory.CommitOutput(Path.Combine(module.Module.OutputDirectory,
#if SOLOAS_UHT_UE54
            // UE 5.4 UEBuildModuleCPP discovers generated translation units by this suffix.
            "sb.soloasbridge.gen.cpp"), output);
#else
            "sb.soloasbridge.cpp"), output);
#endif
    }
    private static void CollectFunctions(
        UhtType type,
        List<FunctionRecord> functions,
        List<DefaultArgumentRecord> defaultArguments,
        List<NativePropertyTraitRecord> propertyTraits,
        List<NativeFunctionTraitRecord> functionTraits,
        List<UhtFunction> referenceCallers,
        bool compiledInOwnerModule,
        bool compiledInConsumerModule = false
    )
    {
        if (type is UhtProperty member && member.Outer is UhtStruct owner && owner is not UhtFunction)
        {
            CollectMemberPropertyTraits(member, owner.SourceName, member.EngineName, propertyTraits);
        }
        if (type is UhtFunction function)
        {
            CollectFunction(
                function,
                functions,
                defaultArguments,
                propertyTraits,
                functionTraits,
                referenceCallers,
                compiledInOwnerModule,
                compiledInConsumerModule
            );
        }
        foreach (UhtType child in type.Children)
        {
            CollectFunctions(
                child,
                functions,
                defaultArguments,
                propertyTraits,
                functionTraits,
                referenceCallers,
                compiledInOwnerModule,
                compiledInConsumerModule
            );
        }
    }
    // Empty Function identifies a native member; suffixes address container subproperties.
    private static void CollectMemberPropertyTraits(
        UhtProperty property, string owner, string path, List<NativePropertyTraitRecord> records,
        ReadOnlyMemory<UhtToken> legacyTokens = default)
    {
        ushort traits = GetNativeDeclarationTraitBits(property);
        if (property.MetaData.ContainsKey("NativeConstTemplateArg"))
            traits |= ConstTemplateArgTrait;
#if SOLOAS_UHT_TYPE_TOKENS
        // Container constructors clear child metadata, but keep the parsed declaration.
        int depth = 0;
        foreach (UhtToken token in property.TypeTokens.DeclarationTokens.Span)
        {
            if (token.IsSymbol("<")) ++depth;
            else if (token.IsSymbol(">")) --depth;
            else if (token.IsIdentifier("const"))
            {
                if (depth == 0) traits |= CppConstTrait;
                else if (depth == 1 && property is UhtObjectPropertyBase)
                    traits |= ConstTemplateArgTrait;
            }
        }
#elif SOLOAS_UHT_LEGACY_DECLARATION_TRAITS
                                                                                                            
        if (property.Outer is UhtStruct && property is UhtMapProperty or UhtArrayProperty or UhtSetProperty)
            legacyTokens = GetLegacyMemberDeclarationTokens(property);
        int depth = 0;
        foreach (UhtToken token in legacyTokens.Span)
        {
            if (token.IsSymbol("<")) ++depth;
            else if (token.IsSymbol(">")) --depth;
            else if (token.IsIdentifier("const"))
            {
                if (depth == 0) traits |= CppConstTrait;
                else if (depth == 1 && property is UhtObjectPropertyBase)
                    traits |= ConstTemplateArgTrait;
            }
        }
                                                                                                          
#endif
        if (traits != 0)
            records.Add(new NativePropertyTraitRecord(owner, "", path, 0, traits));
        if (property is UhtMapProperty map)
        {
            CollectMemberPropertyTraits(map.KeyProperty, owner, path + "/Key", records, LegacyTemplateArgument(legacyTokens, 0));
            CollectMemberPropertyTraits(map.ValueProperty, owner, path + "/Value", records, LegacyTemplateArgument(legacyTokens, 1));
        }
        else if (property is UhtArrayProperty array)
            CollectMemberPropertyTraits(array.ValueProperty, owner, path + "/Inner", records, LegacyTemplateArgument(legacyTokens, 0));
        else if (property is UhtSetProperty set)
            CollectMemberPropertyTraits(set.ValueProperty, owner, path + "/Element", records, LegacyTemplateArgument(legacyTokens, 0));
    }
                                                                                                        
    private static ReadOnlyMemory<UhtToken> LegacyTemplateArgument(ReadOnlyMemory<UhtToken> tokens, int ordinal)
    {
#if SOLOAS_UHT_LEGACY_DECLARATION_TRAITS
        int depth = 0, start = 0, argument = 0;
        for (int i = 0; i < tokens.Length; ++i)
        {
            UhtToken token = tokens.Span[i];
            if (token.IsSymbol("<")) { if (++depth == 1) start = i + 1; }
            else if (token.IsSymbol(">"))
            {
                if (--depth == 0) return argument == ordinal ? tokens[start..i] : default;
            }
            else if (token.IsSymbol(",") && depth == 1)
            {
                if (argument++ == ordinal) return tokens[start..i];
                start = i + 1;
            }
        }
#endif
        return default;
    }
#if SOLOAS_UHT_LEGACY_DECLARATION_TRAITS
    private static ReadOnlyMemory<UhtToken> GetLegacyMemberDeclarationTokens(UhtProperty property)
    {
#if SOLOAS_UHT_SEGMENT_TOKENS
        return property.TypeTokens.Segments.IsEmpty ? default : property.TypeTokens.MyTokens;
#else
        // The parsed property's exact source line selects the active declaration, even
        // when another preprocessor branch declares a member with the same name.
        ReadOnlyMemory<char> source = property.HeaderFile.Data.Memory;
        int offset = 0;
        for (int line = 1; line < property.LineNumber && offset < source.Length; ++offset)
            if (source.Span[offset] == '\n') ++line;
        UhtTokenBufferReader reader = new(property, source[offset..]);
        List<UhtToken> tokens = new();
        int parentheses = 0, templates = 0;
        while (!reader.IsEOF)
        {
            UhtToken token = reader.GetToken();
            if (token.IsSymbol(";") || token.IsSymbol("{")) break;
            if (token.IsSymbol("(")) ++parentheses;
            else if (token.IsSymbol(")")) --parentheses;
            else if (parentheses == 0)
            {
                if (token.IsSymbol("<")) ++templates;
                else if (token.IsSymbol(">")) --templates;
                else if (templates == 0 && token.IsIdentifier(property.SourceName)) return tokens.ToArray();
                tokens.Add(token);
            }
        }
        return default;
#endif
    }
#endif
                                                                                                      
    private static void CollectFunction(
        UhtFunction function,
        List<FunctionRecord> functions,
        List<DefaultArgumentRecord> defaultArguments,
        List<NativePropertyTraitRecord> propertyTraits,
        List<NativeFunctionTraitRecord> functionTraits,
        List<UhtFunction> referenceCallers,
        bool compiledInOwnerModule,
        bool compiledInConsumerModule = false
    )
    {
        // === BEGIN CUVISS STUDIO EXTENSION: CS.Bridge.UHT.ScriptMethodReceiverTraits | by Cuviss Studio ===
        // Ordinary native declarations use NativeConst and RefQualifier, not CPF_ReferenceParm.
        bool scriptCallable = function.MetaData.ContainsKey("ScriptCallable");
        bool nativeCallable = function.FunctionFlags.HasAnyFlags(EFunctionFlags.BlueprintCallable
                | EFunctionFlags.BlueprintPure | EFunctionFlags.BlueprintEvent);
        if (!scriptCallable && !nativeCallable)
        // === END CUVISS STUDIO EXTENSION: CS.Bridge.UHT.ScriptMethodReceiverTraits | by Cuviss Studio ===
        {
            return;
        }
        if (function.Outer is not UhtStruct ownerStruct)
        {
            return;
        }
        string owner = ownerStruct.SourceName;
        RequirePortableIdentifier(owner, "owner");
        RequirePortableIdentifier(function.SourceName, "function");
        string? worldContext = GetExportableWorldContextParameter(
            function,
            GetWorldContextParameter(function)
        );
        if (scriptCallable)
        {
            functions.Add(new FunctionRecord(owner, function.SourceName));
        }
        // A: reproduce UE-AS fork GetASFunctionPointers before replacing its caller with reflection.
        ushort functionBits = scriptCallable ? GetNativeFunctionTraitBits(worldContext != null) : (ushort)0;
        if (function.FunctionExportFlags.HasAnyFlags(UhtFunctionExportFlags.CustomThunk)
            || function.FunctionFlags.HasAnyFlags(EFunctionFlags.NetRequest)
            || ownerStruct is UhtClass { NativeInterface: not null }
            || ownerStruct is UhtClass nativeClass && nativeClass.ClassExportFlags.HasAnyFlags(UhtClassExportFlags.NoExport)
            || function.ParameterProperties.Span.ToArray().Any(parameter =>
                parameter is UhtProperty property && (property.IsStaticArray || property is UhtInterfaceProperty)))
        {
            functionBits |= NoNativeCallerFunctionTrait;
        }
        // Native reference callers preserve addresses; TextureGraph uses the approved frame-owned copy.
        bool nativeReferenceCaller = CanGenerateNativeReferenceCaller(function, functionBits, compiledInOwnerModule, compiledInConsumerModule);
        if (nativeReferenceCaller)
            referenceCallers.Add(function);
        if (functionBits != 0 || nativeReferenceCaller)
            functionTraits.Add(new NativeFunctionTraitRecord(owner, function.SourceName, functionBits,
                nativeReferenceCaller ? NativeReferenceCallerName(function) : null,
                nativeReferenceCaller ? NativeReferenceCallerGuard(function) : null));
        if (function.ReturnProperty is UhtProperty returnProperty)
        {
            ushort returnBits = GetNativeDeclarationTraitBits(returnProperty);
            if (returnBits != 0)
                propertyTraits.Add(new NativePropertyTraitRecord(owner, function.SourceName,
                    returnProperty.SourceName, uint.MaxValue, returnBits));
        }
        uint ordinal = 0U;
        foreach (UhtType parameter in function.ParameterProperties.Span)
        {
            if (ReferenceEquals(parameter, function.ReturnProperty))
            {
                continue;
            }
            UhtProperty property = RequireParameterProperty(parameter);
            RequirePortableIdentifier(property.SourceName, "parameter");
            CollectDefaultArgument(
                property,
                scriptCallable,
                owner,
                function.SourceName,
                ordinal,
                defaultArguments
            );
            ushort propertyBits = scriptCallable
                ? GetNativePropertyTraitBits(property, worldContext)
                : GetNativeDeclarationTraitBits(property);
            if (propertyBits != 0)
            {
                propertyTraits.Add(
                    new NativePropertyTraitRecord(
                        owner,
                        function.SourceName,
                        property.SourceName,
                        ordinal,
                        propertyBits
                    )
                );
            }
            ordinal++;
        }
    }
    private static UhtProperty RequireParameterProperty(UhtType parameter) =>
        parameter as UhtProperty
        ?? throw new InvalidOperationException(
            "ParameterProperties contained a non-property."
        );
    private static void CollectDefaultArgument(
        UhtProperty property,
        bool scriptCallable,
        string owner,
        string function,
        uint ordinal,
        List<DefaultArgumentRecord> records
    )
    {
        if (scriptCallable && TryGetScriptDefault(property, out string value))
        {
            records.Add(
                new DefaultArgumentRecord(
                    owner,
                    function,
                    property.SourceName,
                    ordinal,
                    value
                )
            );
        }
    }
    private static string? GetWorldContextParameter(UhtFunction function)
    {
        if (!function.MetaData.TryGetValue("WorldContext", out string? parameter))
        {
            return null;
        }
        if (parameter == null)
        {
            throw new InvalidOperationException(
                "WorldContext metadata has no value."
            );
        }
        RequirePortableIdentifier(parameter, "WorldContext parameter");
        return parameter;
    }
    private static string? GetExportableWorldContextParameter(
        UhtFunction function,
        string? candidate
    )
    {
        if (candidate == null)
        {
            return null;
        }
        uint matchCount = 0U;
        foreach (UhtType parameter in function.ParameterProperties.Span)
        {
            if (ReferenceEquals(parameter, function.ReturnProperty))
            {
                continue;
            }
            UhtProperty property = RequireParameterProperty(parameter);
            if (StringComparer.Ordinal.Equals(property.SourceName, candidate))
            {
                matchCount++;
            }
        }
        return matchCount == 1U ? candidate : null;
    }
    private static ushort GetNativeFunctionTraitBits(bool worldContext) =>
        (ushort)(ScriptCallableFunctionTrait
            | (worldContext ? WorldContextFunctionTrait : 0));
    private static ushort GetNativePropertyTraitBits(
        UhtProperty property,
        string? worldContext
    )
    {
        ushort traits = 0;
        traits |= property.MetaData.ContainsKey("NativeConst")
            || property.PropertyFlags.HasAnyFlags(EPropertyFlags.ConstParm)
            ? CppConstTrait
            : (ushort)0;
        traits |= property.RefQualifier != UhtPropertyRefQualifier.None
            || property.PropertyFlags.HasAnyFlags(EPropertyFlags.ReferenceParm)
            ? CppRefTrait
            : (ushort)0;
        traits |= property is UhtByteProperty { Enum: not null }
            ? CppEnumAsByteTrait
            : (ushort)0;
        traits |= property.MetaData.ContainsKey("NativeConstTemplateArg")
            ? ConstTemplateArgTrait
            : (ushort)0;
        if (
            worldContext != null
            && StringComparer.Ordinal.Equals(property.SourceName, worldContext)
        )
        {
            traits |= WorldContextTrait;
        }
        return traits;
    }
    // === BEGIN CUVISS STUDIO EXTENSION: CS.Bridge.UHT.ScriptMethodReceiverTraits.Helper | by Cuviss Studio ===
    private static ushort GetNativeDeclarationTraitBits(UhtProperty property)
    {
        ushort traits = (ushort)((property.MetaData.ContainsKey("NativeConst") || property.RefQualifier == UhtPropertyRefQualifier.ConstRef ? CppConstTrait : 0)
            | (property.RefQualifier != UhtPropertyRefQualifier.None ? CppRefTrait : 0)
            | (property is UhtByteProperty { Enum: not null } ? CppEnumAsByteTrait : 0));
#if SOLOAS_UHT_SEGMENT_TOKENS && SOLOAS_UHT_LEGACY_DECLARATION_TRAITS
                                                                                                               
        // UE 5.6 may lose RefQualifier; inspect the declaration, not CPF_ReferenceParm (UPARAM(ref) can be by value).
        if (property.Outer is UhtFunction && !property.TypeTokens.Segments.IsEmpty)
        {
            int depth = 0;
            foreach (UhtToken token in property.TypeTokens.MyTokens.Span)
            {
                if (token.IsSymbol("(") || token.IsSymbol("<")) ++depth;
                else if (token.IsSymbol(")") || token.IsSymbol(">")) --depth;
                else if (depth == 0 && token.IsIdentifier("const")) traits |= CppConstTrait;
                else if (depth == 0 && token.IsSymbol("&")) traits |= CppRefTrait;
            }
        }
                                                                                                             
#endif
        return traits;
    }
    // === END CUVISS STUDIO EXTENSION: CS.Bridge.UHT.ScriptMethodReceiverTraits.Helper | by Cuviss Studio ===
                                                                                                      
    private static bool TryGetScriptDefault(
        UhtProperty property,
        out string value
    )
    {
        value = string.Empty;
        UhtToken[]? defaultTokens = property.DefaultValueTokens?.ToArray();
#if SOLOAS_UHT_LEGACY_SCRIPT_DEFAULTS
        defaultTokens ??= GetLegacyScriptDefaultTokens(property);
#endif
        if (defaultTokens == null)
        {
            return false;
        }
        // Use the same sanitizer as UE-AS fork UhtFunction.ResolveSelf for ScriptCallable.
        StringBuilder builder = new();
#if SOLOAS_UHT_REPLAY_READER_BORROWER
        using UhtTokenReplayReaderBorrower reader = new(property, property.HeaderFile.Data.Memory,
            defaultTokens, UhtTokenType.EndOfDefault);
        if (!property.SanitizeDefaultValue(reader.Reader, builder) || !reader.Reader.IsEOF)
#else
        UhtTokenReplayReader reader = new(property, property.HeaderFile.Data.Memory,
            defaultTokens, UhtTokenType.EndOfDefault);
        if (!property.SanitizeDefaultValue(reader, builder) || !reader.IsEOF)
#endif
        {
            return false;
        }
        value = builder.ToString();
        return true;
    }
#if SOLOAS_UHT_LEGACY_SCRIPT_DEFAULTS
    private static UhtToken[]? GetLegacyScriptDefaultTokens(UhtProperty property)
    {
        if (property.Outer is not UhtFunction function
            || !function.MetaData.ContainsKey("ScriptCallable")
            || function.Outer is not UhtClass { Declarations: not null } owner)
            return null;

        // UHT records the parsed declaration even when it discards DefaultValueTokens.
        // Match the function object so overloads and preprocessor branches cannot alias.
        foreach (UhtDeclaration declaration in owner.Declarations)
        {
            if (!ReferenceEquals(declaration.Function, function))
                continue;
            UhtToken[] tokens = declaration.Tokens;
            int index = 0;
            while (index + 1 < tokens.Length
                && !(tokens[index].IsIdentifier(function.SourceName) && tokens[index + 1].IsSymbol('(')))
                ++index;
            index += 2;
            int depth = 0;
            for (; index < tokens.Length; ++index)
            {
                UhtToken token = tokens[index];
                if (token.IsSymbol(')') && depth == 0)
                    break;
                if (token.IsSymbol('(')) ++depth;
                else if (token.IsSymbol(')')) --depth;
                else if (depth == 0 && token.IsSymbol('=') && index > 0
                    && tokens[index - 1].IsIdentifier(property.SourceName))
                {
                    int start = ++index;
                    // Use the same parenthesis nesting rule as UHT's parameter parser.
                    for (; index < tokens.Length; ++index)
                    {
                        token = tokens[index];
                        if (depth == 0 && (token.IsSymbol(',') || token.IsSymbol(')')))
                            break;
                        if (token.IsSymbol('(')) ++depth;
                        else if (token.IsSymbol(')')) --depth;
                    }
                    return index > start ? tokens[start..index] : null;
                }
            }
            return null;
        }
        return null;
    }
#endif
                                                                                                    
    private static void SortAndRejectDuplicates<T>(
        List<T> records,
        string keyName
    ) where T : IKeyedRecord
    {
        records.Sort((left, right) =>
            StringComparer.Ordinal.Compare(left.SortKey, right.SortKey)
        );
        for (int index = 1; index < records.Count; ++index)
        {
            if (
                StringComparer.Ordinal.Equals(
                    records[index - 1].DuplicateKey,
                    records[index].DuplicateKey
                )
            )
            {
                throw new InvalidOperationException(
                    $"Duplicate {keyName} key."
                );
            }
        }
    }
    private static StringBuilder BuildInline(
        IReadOnlyList<FunctionRecord> functions,
        IReadOnlyList<DefaultArgumentRecord> defaultArguments,
        IReadOnlyList<NativePropertyTraitRecord> propertyTraits,
        IReadOnlyList<NativeFunctionTraitRecord> functionTraits
    )
    {
        StringBuilder builder = new();
        builder.Append("static constexpr FSoloASBridgeScriptFunctionDescriptor ")
            .Append("GeneratedScriptFunctions[] =\n{\n");
        SoloASBridgeExporterSupport.AppendFunctions(builder, functions);
        builder.Append("};\n\n")
            .Append("static constexpr uint32 GeneratedScriptFunctionCount = ")
            .Append(functions.Count).Append("U;\n\n")
            .Append("static constexpr FSoloASBridgeScriptDefaultArgumentDescriptor ")
            .Append("GeneratedScriptDefaultArguments[] =\n{\n");
        AppendDefaultArguments(builder, defaultArguments);
        builder.Append("};\n\n")
            .Append("static constexpr uint32 GeneratedScriptDefaultArgumentCount = ")
            .Append(defaultArguments.Count).Append("U;\n\n")
            .Append("static constexpr FSoloASBridgeNativeFunctionPropertyTraitDescriptor ")
            .Append("GeneratedNativeFunctionPropertyTraits[] =\n{\n");
        AppendPropertyTraits(builder, propertyTraits);
        builder.Append("};\n\n")
            .Append("static constexpr uint32 GeneratedNativeFunctionPropertyTraitCount = ")
            .Append(propertyTraits.Count).Append("U;\n\n")
            .Append("static constexpr FSoloASBridgeNativeFunctionTraitDescriptor ")
            .Append("GeneratedNativeFunctionTraits[] =\n{\n");
        AppendFunctionTraits(builder, functionTraits);
        return builder.Append("};\n\n")
            .Append("static constexpr uint32 GeneratedNativeFunctionTraitCount = ")
            .Append(functionTraits.Count).Append("U;\n");
    }
    private static void AppendDefaultArguments(StringBuilder builder, IReadOnlyList<DefaultArgumentRecord> records)
    {
        if (records.Count == 0)
        {
            builder.Append("    { nullptr, nullptr, nullptr, 0U, nullptr },\n");
            return;
        }
        foreach (DefaultArgumentRecord record in records)
        {
            builder.Append("    { ");
            AppendOctalStringLiteral(builder, record.Owner).Append(", ");
            AppendOctalStringLiteral(builder, record.Function).Append(", ");
            AppendOctalStringLiteral(builder, record.Parameter).Append(", ");
            builder.Append(record.ParameterOrdinal).Append("U, ");
            AppendOctalStringLiteral(builder, record.Value).Append(" },\n");
        }
    }
    private static void AppendPropertyTraits(StringBuilder builder, IReadOnlyList<NativePropertyTraitRecord> records)
    {
        if (records.Count == 0)
        {
            builder.Append(
                "    { nullptr, nullptr, nullptr, 0U, "
                    + "ESoloASBridgePropertyTrait::None },\n"
            );
            return;
        }
        foreach (NativePropertyTraitRecord record in records)
        {
            builder.Append("    { ");
            AppendOctalStringLiteral(builder, record.Owner).Append(", ");
            AppendOctalStringLiteral(builder, record.Function).Append(", ");
            AppendOctalStringLiteral(builder, record.Property).Append(", ");
            builder.Append(record.ParameterOrdinal)
                .Append("U, static_cast<ESoloASBridgePropertyTrait>(")
                .Append(record.TraitBits)
                .Append("U) },\n");
        }
    }
    private static void AppendFunctionTraits(StringBuilder builder, IReadOnlyList<NativeFunctionTraitRecord> records)
    {
        if (records.Count == 0)
        {
            builder.Append(
                "    { nullptr, nullptr, "
                    + "ESoloASBridgeFunctionTrait::None },\n"
            );
            return;
        }
        foreach (NativeFunctionTraitRecord record in records)
        {
            builder.Append("    { ");
            AppendOctalStringLiteral(builder, record.Owner).Append(", ");
            AppendOctalStringLiteral(builder, record.Function);
            builder.Append(", static_cast<ESoloASBridgeFunctionTrait>(")
                .Append(record.TraitBits).Append("U), ");
            if (record.CallerGuard != null)
                builder.Append("\n#if ").Append(record.CallerGuard).Append('\n');
            builder.Append(record.Caller ?? "nullptr");
            if (record.CallerGuard != null)
                builder.Append("\n#else\nnullptr\n#endif\n");
            builder.Append(" },\n");
        }
    }
    // === BEGIN CUVISS STUDIO EXTENSION: CS.Bridge.UHT.NativeReferenceReturn | by Cuviss Studio ===
    private static bool CanGenerateNativeReferenceCaller(UhtFunction function, ushort functionBits, bool compiledInOwnerModule, bool compiledInConsumerModule)
    {
// CS.Bridge.UHT.LegacyQualityLevel55: exact void ref/out native-thunk route.
#if SOLOAS_UHT_UE55
        // Engine/Classes/Engine/{SkeletalMesh,StaticMesh}.h uses non-exported
        // QualityLevelProperty::ConvertQualityLevelData<int> on 5.5. These void
        // ref/out calls retain native caller storage through FFrame::OutParms.
        if (function.Outer?.SourceName is "USkeletalMesh" or "UStaticMesh"
            && function.SourceName is "SetMinLODForQualityLevels" or "GetMinLODForQualityLevels")
            return false;
#endif
        bool referenceReturn = function.ReturnProperty is UhtProperty result
            && (GetNativeDeclarationTraitBits(result) & CppRefTrait) != 0;
        bool typedValueContract = NativeCallProperties(function).Any(RequiresTypedValueContract);
        if ((functionBits & NoNativeCallerFunctionTrait) != 0
            || (!referenceReturn && !typedValueContract)
            || !function.FunctionFlags.HasAnyFlags(EFunctionFlags.Native)
            || function.Outer is not UhtClass owner
            || !function.FunctionFlags.HasAnyFlags(EFunctionFlags.Public | EFunctionFlags.Protected | EFunctionFlags.Private)
            || function.FunctionFlags.HasAnyFlags(EFunctionFlags.Event | EFunctionFlags.BlueprintEvent | EFunctionFlags.Net
                | EFunctionFlags.Delegate | EFunctionFlags.MulticastDelegate))
            return false;
        // MinimalAPI does not export ordinary declarations; an inline body remains callable.
        if (!compiledInOwnerModule && !IsPythonDelegateReferenceReturn(function) && !IsPcgSelectorReferenceReturn(function)
            && !HasExactStorageCaller(function) && !IsAvalancheMaterialResidual(function) && !IsTextureGraphRenderResidual(function)
            && !owner.ClassFlags.HasAnyFlags(EClassFlags.RequiredAPI)
            && !function.FunctionExportFlags.HasAnyFlags(UhtFunctionExportFlags.RequiredAPI)
            && !HasInlineNativeBody(function))
            return false;
        // Installed engine modules are not rebuilt. Stay in Bridge's existing link closure.
        string module =
#if SOLOAS_UHT_UE54
            function.HeaderFile.Package.ShortName;
#else
            function.HeaderFile.Module.ShortName;
#endif
        return (compiledInOwnerModule || compiledInConsumerModule || module == "Core" || module == "CoreUObject" || module == "Engine")
            && (function.DefineScope.ToString() == "None"
                || function.DefineScope.ToString() == "Editor"
                || function.DefineScope.ToString() == "EditorOnlyData");
    }
                                                                                                     
    private static string? ExactStorageField(UhtFunction function) => (function.Outer?.SourceName, function.SourceName) switch
    {
        ("UDMMaterialStageBlend", "GetBlendDescription") => "BlendDescription",
        ("UDynamicMaterialModelEditorOnlyData", "GetMaterialStats") => "MaterialStats",
        ("UMovieGraphEvaluatedConfig", "GetEvaluatedGraphs") => "EvaluatedGraphs",
        ("UText3DStaticMeshesRenderer", "GetGlyphMeshComponents") => "CharacterMeshes",
        ("UOptimusDeformerInstance", "GetVariables") => "Variables",
        _ => null,
    };
    private static bool IsProjectDirectoriesGetter(UhtFunction function) =>
        function.Outer?.SourceName == "UPluginBlueprintLibrary" && function.SourceName == "GetAdditionalProjectPluginSearchPaths";
    private static bool HasExactStorageCaller(UhtFunction function) =>
        ExactStorageField(function) != null || IsProjectDirectoriesGetter(function);
                                                                                                   
                                                                                                         
    private static bool IsAvalancheMaterialResidual(UhtFunction function) =>
        function.Outer?.SourceName == "UAvaShapeDynamicMeshBase" && function.SourceName == "GetParametricMaterial";
                                                                                                       
                                                                                                             
    private static bool IsTextureGraphRenderResidual(UhtFunction function) =>
        function.Outer?.SourceName == "UTG_BlueprintFunctionLibrary" && function.SourceName == "RenderTextureGraph";
                                                                                                           
    private static IEnumerable<UhtProperty> NativeCallProperties(UhtFunction function)
    {
        foreach (UhtProperty parameter in function.ParameterProperties.Span.ToArray().OfType<UhtProperty>())
            yield return parameter;
        if (function.ReturnProperty is UhtProperty result) yield return result;
    }
                                                                                                
    private static bool IsSkeletalQualityLevelAccessor(UhtFunction function) =>
#if SOLOAS_UHT_UE54
        function.Outer?.SourceName == "USkeletalMesh"
        && function.SourceName is "SetMinLODForQualityLevels" or "GetMinLODForQualityLevels";
#else
        false;
#endif
                                                                                              
                                                                                              
    private static bool IsStaticQualityLevelAccessor(UhtFunction function) =>
#if SOLOAS_UHT_UE54
        function.Outer?.SourceName == "UStaticMesh"
        && function.SourceName is "SetMinLODForQualityLevels" or "GetMinLODForQualityLevels";
#else
        false;
#endif
                                                                                            
    private static bool IsPhysicsControlPureConversion(UhtFunction function) =>
        function.Outer?.SourceName == "UPhysicsControlBPLibrary"
        && function.SourceName == "ConvertToRigidBodyWithControlPure";
                                                                                              
    private static bool IsMvvmCounterAccessor(UhtFunction function) =>
        function.Outer?.SourceName == "UMVVMViewModelBindingExecTest"
        && function.SourceName is "GetterA" or "SetterA" or "GetterB" or "SetterB"
        && function.HeaderFile.ModuleRelativeFilePath == "Private/Tests/MVVMBindingExecuteTest.h";
                                                                                            
    private static bool RequiresTypedValueContract(UhtProperty property)
    {
        // Plain scalar and raw-object slots have no C++ value lifetime to preserve.
        // Structs, containers and wrappers must use the same typed call as the fork.
        string type = NativeType(property, false).Replace("const ", "", StringComparison.Ordinal).Trim();
        return type is not ("bool" or "int8" or "int16" or "int32" or "int64"
            or "uint8" or "uint16" or "uint32" or "uint64" or "float" or "double")
            && !(property is UhtObjectProperty && type.EndsWith("*", StringComparison.Ordinal));
    }
    private static IEnumerable<UhtHeaderFile> NativeValueHeaders(UhtProperty property)
    {
        if (property is UhtStructProperty structure)
            yield return structure.ScriptStruct.HeaderFile;
        else if (property is UhtArrayProperty array)
            foreach (UhtHeaderFile header in NativeValueHeaders(array.ValueProperty)) yield return header;
        else if (property is UhtSetProperty set)
            foreach (UhtHeaderFile header in NativeValueHeaders(set.ValueProperty)) yield return header;
        else if (property is UhtMapProperty map)
        {
            foreach (UhtHeaderFile header in NativeValueHeaders(map.KeyProperty)) yield return header;
            foreach (UhtHeaderFile header in NativeValueHeaders(map.ValueProperty)) yield return header;
        }
    }
                                                                                                       
    private static bool IsPythonDelegateReferenceReturn(UhtFunction function) =>
        function.Outer?.SourceName == "UPyTestTypeHint"
        && function.SourceName == "CheckDelegateTypeHints"
        && function.ReturnProperty is UhtDelegateProperty
        && function.HeaderFile.ModuleRelativeFilePath == "Private/PyTest.h";
                                                                                                     
                                                                                                    
    private static bool IsPcgSelectorReferenceReturn(UhtFunction function) =>
        function.Outer?.SourceName == "UPCGAttributePropertySelectorBlueprintHelpers"
        && function.SourceName == "GetExtraNames"
        && function.HeaderFile.ModuleRelativeFilePath == "Public/Metadata/PCGAttributePropertySelector.h";
                                                                                                  
    private static string NativeReferenceCallerName(UhtFunction function) =>
        "SoloASNativeReference_" + function.Outer!.SourceName + "_" + function.SourceName;
    private static string? NativeReferenceCallerGuard(UhtFunction function) =>
#if SOLOAS_UHT_UE54
        // UE 5.4 folds WITH_EDITOR into EditorOnlyData in DefineScope.
        function.FunctionFlags.HasAnyFlags(EFunctionFlags.EditorOnly) ? "WITH_EDITOR" :
#endif
        function.DefineScope.ToString() == "Editor" ? "WITH_EDITOR"
        : function.DefineScope.ToString() == "EditorOnlyData" ? "WITH_EDITORONLY_DATA" : null;
    private static bool HasInlineNativeBody(UhtFunction function)
    {
        // UHT's declaration recorder omits inline bodies. Replay the actual declaration tokens.
        ReadOnlyMemory<char> source = function.HeaderFile.Data.Memory;
        int start = 0;
        for (int line = 1; line < function.LineNumber; ++line)
        {
            int newline = source.Span[start..].IndexOf('\n');
            if (newline < 0) return false;
            start += newline + 1;
        }
        UhtTokenBufferReader reader = new(function, source[start..]);
        while (!reader.IsEOF)
        {
            UhtToken token = reader.GetToken();
            if (!token.IsIdentifier(function.SourceName) || !reader.TryOptional('(')) continue;
            int depth = 1;
            while (!reader.IsEOF && depth != 0)
            {
                token = reader.GetToken();
                if (token.IsSymbol('(')) ++depth;
                else if (token.IsSymbol(')')) --depth;
            }
            while (!reader.IsEOF)
            {
                token = reader.GetToken();
                if (token.IsSymbol('{')) return true;
                if (token.IsSymbol(';')) return false;
            }
        }
        return false;
    }
    private static string NativeClassName(UhtClass owner) =>
#if SOLOAS_UHT_NAMESPACES
        owner.Namespace.FullSourceName + owner.SourceName;
#else
        owner.SourceName;
#endif
    private static string NativeType(UhtProperty property, bool includeReference = true)
    {
        StringBuilder type = new();
        ushort traits = GetNativeDeclarationTraitBits(property);
        if ((traits & CppConstTrait) != 0) type.Append("const ");
        if (property is UhtDelegateProperty delegateProperty && delegateProperty.Function.Outer is UhtClass delegateOwner)
            type.Append(NativeClassName(delegateOwner)).Append("::");
        if (property is UhtMulticastDelegateProperty multicastProperty && multicastProperty.Function.Outer is UhtClass multicastOwner)
            type.Append(NativeClassName(multicastOwner)).Append("::");
        if (property is UhtByteProperty { Enum: not null } byteProperty && (traits & CppEnumAsByteTrait) != 0)
            type.Append("TEnumAsByte<").Append(byteProperty.Enum.CppType).Append('>');
        else
            property.AppendText(type, UhtPropertyTextType.GenericFunctionArgOrRetVal);
        if (property.MetaData.ContainsKey("PointerConst")) type.Append(" const");
        if (includeReference && (traits & CppRefTrait) != 0) type.Append('&');
        return type.ToString();
    }
    private static StringBuilder BuildNativeReferenceCallers(IUhtExportFactory factory, List<UhtFunction> functions)
    {
        StringBuilder output = new("// CS.Bridge.UHT.NativeReferenceReturn\n");
        if (functions.Any(IsTextureGraphRenderResidual))
            output.Append("#include \"Runtime/SoloASBridgeCallableDispatch.h\"\n");
        if (functions.Any(IsPythonDelegateReferenceReturn))
            output.Append("#include \"UObject/UnrealType.h\"\n");
        // Use the same include path as UHT's native .gen.cpp exporter.
        foreach (string header in functions.Where(function => !IsPythonDelegateReferenceReturn(function))
            .SelectMany(function => new[] { function.HeaderFile }.Concat(NativeCallProperties(function).SelectMany(NativeValueHeaders)))
#if SOLOAS_UHT_UE54
            .Select(header => Path.GetRelativePath(header.Package.Module.IncludeBase,
                header.FilePath).Replace('\\', '/'))
#elif SOLOAS_UHT_LEGACY_INCLUDE_PATH
            .Select(header => Path.GetRelativePath(header.Module.Module.IncludeBase,
                header.FilePath).Replace('\\', '/'))
#else
            .Select(header => factory.GetModuleShortestIncludePath(header.Module,
                header.FilePath))
#endif
            .Distinct().OrderBy(path => path, StringComparer.Ordinal))
        {
#if SOLOAS_UHT_UE57
            // CS.Bridge.UHT.InstancedSkinnedExperimental57: UE-365552 is a misplaced
            // compile annotation, not a native ABI change. Scope suppression to this
            // installed header; keep its original functions and restore the macro.
            bool suppressExperimental = header == "Components/InstancedSkinnedMeshComponent.h";
            if (suppressExperimental)
                output.Append("#pragma push_macro(\"UE_EXPERIMENTAL\")\n#undef UE_EXPERIMENTAL\n#define UE_EXPERIMENTAL(...)\n");
#endif
            output.Append("#include \"").Append(header).Append("\"\n");
#if SOLOAS_UHT_UE57
            if (suppressExperimental)
                output.Append("#pragma pop_macro(\"UE_EXPERIMENTAL\")\n");
#endif
        }
        // These reflected return declarations forward-declare their element types.
        if (functions.Any(function => NativeCallProperties(function)
            .Any(property => NativeType(property).Contains("FPCGLandscapeLayerWeight", StringComparison.Ordinal))))
            output.Append("#include \"Grid/PCGLandscapeCache.h\"\n");
        if (functions.Any(function => NativeCallProperties(function)
            .Any(property => NativeType(property).Contains("FMVVMAvailableBinding", StringComparison.Ordinal))))
            output.Append("#include \"Types/MVVMAvailableBinding.h\"\n");
        if (functions.Any(IsPhysicsControlPureConversion))
            output.Append("#include \"AnimNode_RigidBodyWithControl.h\"\n");
        if (functions.Any(IsProjectDirectoriesGetter))
            output.Append("#include \"Interfaces/IProjectManager.h\"\n");
        if (functions.Any(HasExactStorageCaller))
            output.Append("#include \"UObject/UnrealType.h\"\n#include \"UObject/ObjectPtr.h\"\n");
        if (functions.Any(IsMvvmCounterAccessor))
            output.Append("#include \"UObject/StructOnScope.h\"\n#include \"UObject/UnrealType.h\"\n");
                                                                                                            
#if SOLOAS_UHT_UE54
        if (functions.Any(function => IsSkeletalQualityLevelAccessor(function) || IsStaticQualityLevelAccessor(function)))
            output.Append(@"namespace SoloAS54QualityLevelProperty
{
	template<typename _ValueType>
	TMap<int32, _ValueType> ConvertQualtiyLevelData(const TMap<EPerQualityLevels, _ValueType>& Data)
	{
		TMap<int32, _ValueType> ConvertedData;

		for (const TPair<EPerQualityLevels, int32>& Pair : Data)
		{
			ConvertedData.Add((int32)Pair.Key,Pair.Value);
		}

		return ConvertedData;
	}

	template<typename _ValueType>
	TMap<EPerQualityLevels, _ValueType> ConvertQualtiyLevelData(const TMap<int32, _ValueType>& Data)
	{
		TMap<EPerQualityLevels, _ValueType> ConvertedData;

		for (const TPair<int32, _ValueType>& Pair : Data)
		{
			ConvertedData.Add((EPerQualityLevels)Pair.Key, Pair.Value);
		}

		return ConvertedData;
	}

}
struct FSoloAS54WaitAccess { using FNative = void (USkeletalMesh::*)(ESkeletalMeshAsyncProperties, ESkinnedAssetAsyncPropertyLockType) const; friend FNative Resolve(FSoloAS54WaitAccess); };
template<class T, typename T::FNative Function> struct TSoloAS54WaitGrant { friend typename T::FNative Resolve(T) { return Function; } };
template struct TSoloAS54WaitGrant<FSoloAS54WaitAccess, &USkeletalMesh::WaitUntilAsyncPropertyReleased>;
struct FSoloAS54StaticWaitAccess { using FNative = void (UStaticMesh::*)(EStaticMeshAsyncProperties) const; friend FNative Resolve(FSoloAS54StaticWaitAccess); };
template struct TSoloAS54WaitGrant<FSoloAS54StaticWaitAccess, &UStaticMesh::WaitUntilAsyncPropertyReleased>;
");
#endif
                                                                                                          
        output.Append("PRAGMA_DISABLE_DEPRECATION_WARNINGS\n");
        foreach (UhtFunction function in functions)
        {
            string? guard = NativeReferenceCallerGuard(function);
            if (guard != null) output.Append("#if ").Append(guard).Append('\n');
                                                                                                                      
            if (IsPythonDelegateReferenceReturn(function))
            {
                output.Append("static void ").Append(NativeReferenceCallerName(function))
                    .Append("(UObject* Target, void**, void* ReturnValue)\n{\n")
                    .Append("    FDelegateProperty* Property = FindFProperty<FDelegateProperty>(Target->GetClass(), TEXT(\"DelegateProp\"));\n    check(Property);\n")
                    .Append("    *static_cast<void**>(ReturnValue) = Property->ContainerPtrToValuePtr<void>(Target);\n}\n");
                if (guard != null) output.Append("#endif\n");
                continue;
            }
                                                                                                                    
            if (IsTextureGraphRenderResidual(function))
            {
                output.Append("static void ").Append(NativeReferenceCallerName(function))
                    .Append("(UObject* Target, void** Arguments, void* ReturnValue)\n{\n")
                    .Append("    UFunction* Function = Target->GetClass()->FindFunctionByName(TEXT(\"RenderTextureGraph\"));\n")
                    .Append("    *static_cast<void**>(ReturnValue) = SoloASBridge::InvokeReflectedReturnCopy(Target, Function, Arguments);\n}\n");
                if (guard != null) output.Append("#endif\n");
                continue;
            }
                                                                                                                        
            if (IsAvalancheMaterialResidual(function))
            {
                output.Append("static void ").Append(NativeReferenceCallerName(function))
                    .Append("(UObject* Target, void** Arguments, void* ReturnValue)\n{\n")
                    .Append("    auto* Result = static_cast<UAvaShapeDynamicMeshBase*>(Target)->GetParametricMaterialPtr(*static_cast<int32*>(Arguments[0]));\n")
                    .Append("    static const FAvaShapeParametricMaterial DefaultMaterial;\n")
                    .Append("    *static_cast<void**>(ReturnValue) = Result ? Result : const_cast<FAvaShapeParametricMaterial*>(&DefaultMaterial);\n}\n");
                if (guard != null) output.Append("#endif\n");
                continue;
            }
                                                                                                                      
                                                                                                                    
            if (HasExactStorageCaller(function))
            {
                string ownerName = NativeClassName((UhtClass)function.Outer!);
                string field = ExactStorageField(function) ?? "";
                string resultType = NativeType(function.ReturnProperty!, false);
                if (field == "EvaluatedGraphs")
                {
                    string tag = NativeReferenceCallerName(function) + "_Storage";
                    output.Append("struct ").Append(tag).Append(" { using FField = TArray<UMovieGraphConfig*> UMovieGraphEvaluatedConfig::*; friend FField Resolve(").Append(tag).Append("); };\n")
                        .Append("template<class T, typename T::FField Field> struct ").Append(tag).Append("_Grant { friend typename T::FField Resolve(T) { return Field; } };\n")
                        .Append("template struct ").Append(tag).Append("_Grant<").Append(tag).Append(", &UMovieGraphEvaluatedConfig::EvaluatedGraphs>;\n");
                }
                output.Append("static void ").Append(NativeReferenceCallerName(function))
                    .Append("(UObject* Target, void**, void* ReturnValue)\n{\n");
                if (IsProjectDirectoriesGetter(function))
                    output.Append("    const auto& Result = IProjectManager::Get().GetAdditionalPluginDirectories();\n");
                else if (field == "EvaluatedGraphs")
                    output.Append("    const auto& Result = static_cast<UMovieGraphEvaluatedConfig*>(Target)->*Resolve(").Append(NativeReferenceCallerName(function)).Append("_Storage{});\n");
                else
                {
                    // Look up the native declaration owner, never a script subclass's shadow field.
                    output.Append("    UClass* Owner = FindObjectChecked<UClass>(nullptr, TEXT(\"/Script/")
#if SOLOAS_UHT_UE54
                        .Append(function.HeaderFile.Package.ShortName)
#else
                        .Append(function.HeaderFile.Module.ShortName)
#endif
                        .Append('.').Append(ownerName[1..]).Append("\"));\n")
                        .Append("    FProperty* Field = FindFProperty<FProperty>(Owner, TEXT(\"").Append(field).Append("\"));\n    check(Field);\n");
                    if (field == "Variables")
                        output.Append("    UObject* Container = CastFieldChecked<FObjectPropertyBase>(Field)->GetObjectPropertyValue(Field->ContainerPtrToValuePtr<void>(Target));\n")
                            .Append("    Field = FindFProperty<FProperty>(CastFieldChecked<FObjectPropertyBase>(Field)->PropertyClass, TEXT(\"Descriptions\"));\n    check(Field);\n")
                            .Append("    const auto& Result = *Field->ContainerPtrToValuePtr<").Append(resultType).Append(">(Container);\n");
                    else if (field == "CharacterMeshes")
                        output.Append("    const auto& Result = ObjectPtrDecay(*Field->ContainerPtrToValuePtr<TArray<TObjectPtr<UStaticMeshComponent>>>(Target));\n");
                    else
                        output.Append("    const auto& Result = *Field->ContainerPtrToValuePtr<").Append(resultType).Append(">(Target);\n");
                }
                output.Append("    *static_cast<void**>(ReturnValue) = const_cast<void*>(static_cast<const void*>(&Result));\n}\n");
                if (guard != null) output.Append("#endif\n");
                continue;
            }
                                                                                                                  
                                                                                                             
            if (IsMvvmCounterAccessor(function))
            {
                bool getter = function.SourceName.StartsWith("Getter", StringComparison.Ordinal);
                string field = function.SourceName.EndsWith("A", StringComparison.Ordinal) ? "PropertyA" : "PropertyB";
                // The inline bodies only copy the corresponding property. Construct/Copy/Destruct
                // execute the owning module's counter operations, including array element lifetimes.
                output.Append("static void ").Append(NativeReferenceCallerName(function))
                    .Append("(UObject* Target, void** Arguments, void* ReturnValue)\n{\n")
                    .Append("    UClass* Owner = FindObjectChecked<UClass>(nullptr, TEXT(\"/Script/ModelViewViewModelEditor.MVVMViewModelBindingExecTest\"));\n")
                    .Append("    UFunction* Function = Owner->FindFunctionByName(TEXT(\"").Append(function.SourceName).Append("\"));\n    check(Function);\n")
                    .Append("    FProperty* Field = FindFProperty<FProperty>(Owner, TEXT(\"").Append(field).Append("\"));\n    check(Field);\n")
                    .Append("    FProperty* Slot = FindFProperty<FProperty>(Function, TEXT(\"").Append(getter ? "ReturnValue" : "Value").Append("\"));\n    check(Slot);\n")
                    .Append("    FStructOnScope Temporary(Function);\n    void* Value = Slot->ContainerPtrToValuePtr<void>(Temporary.GetStructMemory());\n");
                if (getter)
                    output.Append("    Slot->CopyCompleteValue(Value, Field->ContainerPtrToValuePtr<void>(Target));\n")
                        .Append("    Slot->InitializeValue(ReturnValue);\n    Slot->CopyCompleteValue(ReturnValue, Value);\n");
                else
                    output.Append("    Slot->CopyCompleteValue(Value, Arguments[0]);\n")
                        .Append("    Field->CopyCompleteValue(Field->ContainerPtrToValuePtr<void>(Target), Value);\n");
                output.Append("}\n");
                if (guard != null) output.Append("#endif\n");
                continue;
            }
                                                                                                           
                                                                                                               
#if SOLOAS_UHT_UE54
            if (IsSkeletalQualityLevelAccessor(function))
            {
                bool setter = function.SourceName == "SetMinLODForQualityLevels";
                output.Append("static void ").Append(NativeReferenceCallerName(function))
                    .Append("(UObject* Target, void** Arguments, void*)\n{\n")
                    .Append("    auto* Mesh = static_cast<USkeletalMesh*>(Target);\n")
                    .Append("    namespace QualityLevelProperty = SoloAS54QualityLevelProperty;\n")
                    .Append("    auto& MinQualityLevelLOD = Mesh->MinQualityLevelLOD;\n")
                    .Append("    auto WaitUntilAsyncPropertyReleased = [Mesh](ESkeletalMeshAsyncProperties Properties, ESkinnedAssetAsyncPropertyLockType Lock = ESkinnedAssetAsyncPropertyLockType::ReadWrite) { (Mesh->*Resolve(FSoloAS54WaitAccess{}))(Properties, Lock); };\n")
                    .Append(setter
                        ? "    const auto& QualityLevelMinimumLODs = *static_cast<const TMap<EPerQualityLevels, int32>*>(Arguments[0]);\n    int32 Default = *static_cast<int32*>(Arguments[1]);\n"
                        : "    auto& QualityLevelMinimumLODs = *static_cast<TMap<EPerQualityLevels, int32>*>(Arguments[0]);\n    int32& Default = *static_cast<int32*>(Arguments[1]);\n")
                    .Append(setter ? @"{
#if WITH_EDITORONLY_DATA
		WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::MinLod);
		MinQualityLevelLOD.PerQuality = QualityLevelProperty::ConvertQualtiyLevelData(QualityLevelMinimumLODs);
		MinQualityLevelLOD.Default = Default >=0 ? Default : MinQualityLevelLOD.Default;
#endif
	}" : @"{
#if WITH_EDITORONLY_DATA
		WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::MinLod, ESkinnedAssetAsyncPropertyLockType::ReadOnly);
		QualityLevelMinimumLODs = QualityLevelProperty::ConvertQualtiyLevelData(MinQualityLevelLOD.PerQuality);
		Default = MinQualityLevelLOD.Default;
#endif
	}").Append("\n}\n");
                if (guard != null) output.Append("#endif\n");
                continue;
            }
#endif
                                                                                                             
                                                                                                             
#if SOLOAS_UHT_UE54
            if (IsStaticQualityLevelAccessor(function))
            {
                bool setter = function.SourceName == "SetMinLODForQualityLevels";
                output.Append("static void ").Append(NativeReferenceCallerName(function))
                    .Append("(UObject* Target, void** Arguments, void*)\n{\n")
                    .Append("    auto* Mesh = static_cast<UStaticMesh*>(Target);\n")
                    .Append("    namespace QualityLevelProperty = SoloAS54QualityLevelProperty;\n")
                    .Append("    auto& MinQualityLevelLOD = Mesh->MinQualityLevelLOD;\n")
                    .Append("    auto WaitUntilAsyncPropertyReleased = [Mesh](EStaticMeshAsyncProperties Properties) { (Mesh->*Resolve(FSoloAS54StaticWaitAccess{}))(Properties); };\n")
                    .Append(setter
                        ? "    const auto& QualityLevelMinimumLODs = *static_cast<const TMap<EPerQualityLevels, int32>*>(Arguments[0]);\n    int32 Default = *static_cast<int32*>(Arguments[1]);\n"
                        : "    auto& QualityLevelMinimumLODs = *static_cast<TMap<EPerQualityLevels, int32>*>(Arguments[0]);\n    int32& Default = *static_cast<int32*>(Arguments[1]);\n")
                    .Append(setter ? @"{
#if WITH_EDITORONLY_DATA
		WaitUntilAsyncPropertyReleased(EStaticMeshAsyncProperties::MinLOD);
		MinQualityLevelLOD.PerQuality = QualityLevelProperty::ConvertQualtiyLevelData(QualityLevelMinimumLODs);
		MinQualityLevelLOD.Default = Default >= 0 ? Default : MinQualityLevelLOD.Default;
#endif
	}" : @"{
#if WITH_EDITORONLY_DATA
		WaitUntilAsyncPropertyReleased(EStaticMeshAsyncProperties::MinLOD);
		QualityLevelMinimumLODs = QualityLevelProperty::ConvertQualtiyLevelData(MinQualityLevelLOD.PerQuality);
		Default = MinQualityLevelLOD.Default;
#endif
	}").Append("\n}\n");
                if (guard != null) output.Append("#endif\n");
                continue;
            }
#endif
                                                                                                           
            if (IsPhysicsControlPureConversion(function))
            {
                // The inline wrapper only delegates to this public conversion template;
                // calling its non-exported out-of-line helper would not link in an installed SDK.
                output.Append("static void ").Append(NativeReferenceCallerName(function))
                    .Append("(UObject*, void** Arguments, void*)\n{\n")
                    .Append("    EAnimNodeReferenceConversionResult Conversion;\n")
                    .Append("    *static_cast<FRigidBodyWithControlReference*>(Arguments[1]) = FAnimNodeReference::ConvertToType<FRigidBodyWithControlReference>(*static_cast<const FAnimNodeReference*>(Arguments[0]), Conversion);\n")
                    .Append("    *static_cast<bool*>(Arguments[2]) = Conversion == EAnimNodeReferenceConversionResult::Succeeded;\n}\n");
                if (guard != null) output.Append("#endif\n");
                continue;
            }
            if (IsPcgSelectorReferenceReturn(function))
            {
                output.Append("static void ").Append(NativeReferenceCallerName(function))
                    .Append("(UObject*, void** Arguments, void* ReturnValue)\n{\n")
                    .Append("    const auto& Result = static_cast<const FPCGAttributePropertySelector*>(Arguments[0])->GetExtraNames();\n")
                    .Append("    *static_cast<void**>(ReturnValue) = const_cast<void*>(static_cast<const void*>(&Result));\n}\n");
                if (guard != null) output.Append("#endif\n");
                continue;
            }
            string owner = NativeClassName((UhtClass)function.Outer!);
            UhtProperty[] parameters = function.ParameterProperties.Span.ToArray()
                .OfType<UhtProperty>().Where(property => !ReferenceEquals(property, function.ReturnProperty)).ToArray();
            string signature = string.Join(", ", parameters.Select(property => NativeType(property)));
            bool isStatic = function.FunctionFlags.HasAnyFlags(EFunctionFlags.Static);
            bool isConst = function.FunctionExportFlags.HasAnyFlags(UhtFunctionExportFlags.DeclaredConst);
            string returnType = function.ReturnProperty is UhtProperty nativeResult ? NativeType(nativeResult) : "void";
            bool returnsReference = function.ReturnProperty is UhtProperty referenceResult
                && (GetNativeDeclarationTraitBits(referenceResult) & CppRefTrait) != 0;
            output.Append("static void ").Append(NativeReferenceCallerName(function))
                .Append("(UObject* Target, void** Arguments, void* ReturnValue)\n{\n    using FNative = ")
                .Append(returnType).Append(isStatic ? " (*) (" : " (" + owner + "::*) (")
                .Append(signature).Append(')');
            if (!isStatic && isConst) output.Append(" const");
            output.Append(";\n");
            if (function.FunctionFlags.HasAnyFlags(EFunctionFlags.Protected))
                output.Append("    struct FProtectedAccess : ").Append(owner)
                    .Append(" { static FNative Resolve() { return static_cast<FNative>(&FProtectedAccess::")
                    .Append(function.CppImplName).Append("); } };\n    auto Native = FProtectedAccess::Resolve();\n");
            else if (function.FunctionFlags.HasAnyFlags(EFunctionFlags.Private))
            {
                string tag = NativeReferenceCallerName(function) + "_Access";
                string declaration = output.ToString();
                int functionStart = declaration.LastIndexOf("static void " + NativeReferenceCallerName(function), StringComparison.Ordinal);
                string pointer = returnType + (isStatic ? " (*) (" : " (" + owner + "::*) (")
                    + signature + ")" + (!isStatic && isConst ? " const" : "");
                StringBuilder access = new();
                access.Append("struct ").Append(tag).Append(" { using FNative = ").Append(pointer)
                    .Append("; friend FNative Resolve(").Append(tag).Append("); };\n")
                    .Append("template<class T, typename T::FNative Function> struct ").Append(tag).Append("_Grant { friend typename T::FNative Resolve(T) { return Function; } };\n")
                    .Append("template struct ").Append(tag).Append("_Grant<").Append(tag).Append(", static_cast<").Append(tag).Append("::FNative>(&")
                    .Append(owner).Append("::").Append(function.CppImplName).Append(")>;\n");
                output.Insert(functionStart, access.ToString());
                output.Append("    auto Native = static_cast<FNative>(Resolve(").Append(tag).Append("{}));\n");
            }
            else
                output.Append("    auto Native = static_cast<FNative>(&").Append(owner).Append("::")
                    .Append(function.CppImplName).Append(");\n");
            output.Append(returnType == "void" ? "    " : returnsReference ? "    auto& Result = " : "    const auto& Result = ");
            if (isStatic) output.Append("Native(");
            else output.Append("(static_cast<").Append(isConst ? "const " : "").Append(owner)
                .Append("*>(Target)->*Native)(");
            for (int index = 0; index < parameters.Length; ++index)
            {
                if (index != 0) output.Append(", ");
                UhtProperty property = parameters[index];
                string type = NativeType(property, false);
                bool rawObject = property is UhtObjectProperty && type.TrimEnd().EndsWith("*", StringComparison.Ordinal)
                    && property.RefQualifier == UhtPropertyRefQualifier.None;
                output.Append(rawObject ? "static_cast<" : "*static_cast<").Append(type)
                    .Append(rawObject ? ">(Arguments[" : "*>(Arguments[").Append(index).Append("])");
            }
            output.Append(");\n");
            if (returnsReference)
                output.Append("    *static_cast<void**>(ReturnValue) = const_cast<void*>(static_cast<const void*>(&Result));\n");
            else if (returnType != "void")
                // Fork ASAutoCaller::GetAddressIfReference<T> returns const T& for values.
                output.Append("    new(ReturnValue) ").Append(NativeType(function.ReturnProperty!, false)).Append("(Result);\n");
            output.Append("}\n");
            if (guard != null) output.Append("#endif\n");
        }
        return output.Append("PRAGMA_ENABLE_DEPRECATION_WARNINGS\n");
    }
    // === END CUVISS STUDIO EXTENSION: CS.Bridge.UHT.NativeReferenceReturn | by Cuviss Studio ===
    private static StringBuilder AppendOctalStringLiteral(StringBuilder builder, string value)
    {
        builder.Append('"');
        foreach (char character in value)
        {
            if (character < ' ' || character > '~')
            {
                throw new InvalidOperationException(
                    "Generated text is not printable ASCII."
                );
            }
            int byteValue = character;
            builder.Append('\\')
                .Append((char)('0' + ((byteValue >> 6) & 7)))
                .Append((char)('0' + ((byteValue >> 3) & 7)))
                .Append((char)('0' + (byteValue & 7)));
        }
        return builder.Append('"');
    }
    private static void RequirePortableIdentifier(string value, string field)
    {
        if (
            value.Length == 0
            || !value.All(character =>
                character == '_' || ((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9'))
            )
        )
        {
            throw new InvalidOperationException(
                $"Script {field} is not a portable identifier."
            );
        }
    }
    internal interface IKeyedRecord { string SortKey { get; } string DuplicateKey => SortKey; }
    internal readonly record struct FunctionRecord(string Owner, string Name) : IKeyedRecord
    {
        public string SortKey => $"{Owner}\0{Name}";
    }
    private readonly record struct DefaultArgumentRecord(
        string Owner,
        string Function,
        string Parameter,
        uint ParameterOrdinal,
        string Value
    ) : IKeyedRecord
    {
        public string SortKey =>
            $"{Owner}\0{Function}\0{ParameterOrdinal:D10}\0{Parameter}";
        public string DuplicateKey => $"{Owner}\0{Function}\0{ParameterOrdinal:D10}";
    }
    private readonly record struct NativePropertyTraitRecord(
        string Owner,
        string Function,
        string Property,
        uint ParameterOrdinal,
        ushort TraitBits
    ) : IKeyedRecord
    {
        public string SortKey =>
            $"{Owner}\0{Function}\0{ParameterOrdinal:D10}\0{Property}";
    }
    private readonly record struct NativeFunctionTraitRecord(
        string Owner, string Function, ushort TraitBits, string? Caller = null, string? CallerGuard = null
    ) : IKeyedRecord
    {
        public string SortKey => $"{Owner}\0{Function}";
    }
}
