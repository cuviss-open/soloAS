using System;
using System.Collections.Generic;
using System.Linq;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using EpicGames.Core;
using EpicGames.UHT.Tables;
using EpicGames.UHT.Tokenizer;
using EpicGames.UHT.Types;
using EpicGames.UHT.Utils;
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
    [UhtExporter(
        Name = "SoloASBridgeMetadata",
        Description = "Export module-owned script metadata through Core.",
        ModuleName = "SoloASBridge",
        Options = UhtExporterOptions.Default | UhtExporterOptions.CompileOutput,
        CppFilters = new[] { "*.soloasbridge.cpp" },
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
        List<UhtModule> modules = factory.Session.Modules.Where(module =>
        {
            DirectoryReference source = new(module.Module.BaseDirectory);
            return !source.IsUnderDirectory(engineSource) && !source.IsUnderDirectory(enginePlugins);
        }).ToList();
        ExportHeaders(factory, factory.Session.HeaderFiles.Where(header =>
            !modules.Contains(header.Module)), null);
        foreach (UhtModule module in modules.OrderBy(item => item.ShortName, StringComparer.Ordinal))
            ExportHeaders(factory, module.Headers, module);
    }
    private static void ExportHeaders(
        IUhtExportFactory factory, IEnumerable<UhtHeaderFile> headers, UhtModule? module)
    {
        List<FunctionRecord> functions = new();
        List<DefaultArgumentRecord> defaultArguments = new();
        List<NativePropertyTraitRecord> propertyTraits = new();
        List<NativeFunctionTraitRecord> functionTraits = new();
        foreach (UhtHeaderFile header in headers.OrderBy(
            item => item.ModuleRelativeFilePath, StringComparer.Ordinal))
        {
            foreach (UhtType child in header.Children)
            {
                CollectFunctions(child, functions, defaultArguments, propertyTraits, functionTraits);
            }
        }
        SortAndRejectDuplicates(functions, "script function");
        SortAndRejectDuplicates(defaultArguments, "ScriptCallable default argument");
        SortAndRejectDuplicates(propertyTraits, "native function property trait");
        SortAndRejectDuplicates(functionTraits, "native function trait");
        StringBuilder output = BuildInline(functions, defaultArguments, propertyTraits, functionTraits);
        if (module == null)
        {
            factory.CommitOutput(factory.MakePath(OutputBaseName, OutputExtension), output);
            return;
        }
        string providerPath = Path.Combine(factory.PluginModule!.BaseDirectory,
            "Public", "Shared", "SoloASBridgeScriptMetadataProvider.h");
        factory.AddExternalDependency(providerPath);
        string providerName = module.ShortName + ".soloasbridge.types.h";
        factory.CommitOutput(Path.Combine(module.Module.OutputDirectory, providerName),
            new StringBuilder(File.ReadAllText(providerPath)));
        output.Insert(0, $"#include \"{providerName}\"\nnamespace SoloASBridgeGenerated_{module.ShortName} {{\n");
        output.Append("static FSoloASBridgeScriptMetadataRegistration Registration(\n    \"")
            .Append(module.ShortName).Append("\",\n")
            .Append("    MakeArrayView(GeneratedScriptFunctions, GeneratedScriptFunctionCount),\n")
            .Append("    MakeArrayView(GeneratedScriptDefaultArguments, GeneratedScriptDefaultArgumentCount),\n")
            .Append("    MakeArrayView(GeneratedNativeFunctionPropertyTraits, GeneratedNativeFunctionPropertyTraitCount),\n")
            .Append("    MakeArrayView(GeneratedNativeFunctionTraits, GeneratedNativeFunctionTraitCount));\n}\n");
        // Plugin MakePath always redirects to PluginModule, even with a module argument.
        factory.CommitOutput(Path.Combine(module.Module.OutputDirectory,
            module.ShortName + ".soloasbridge.cpp"), output);
    }
    private static void CollectFunctions(
        UhtType type,
        List<FunctionRecord> functions,
        List<DefaultArgumentRecord> defaultArguments,
        List<NativePropertyTraitRecord> propertyTraits,
        List<NativeFunctionTraitRecord> functionTraits
    )
    {
        if (type is UhtFunction function)
        {
            CollectFunction(
                function,
                functions,
                defaultArguments,
                propertyTraits,
                functionTraits
            );
        }
        foreach (UhtType child in type.Children)
        {
            CollectFunctions(
                child,
                functions,
                defaultArguments,
                propertyTraits,
                functionTraits
            );
        }
    }
    private static void CollectFunction(
        UhtFunction function,
        List<FunctionRecord> functions,
        List<DefaultArgumentRecord> defaultArguments,
        List<NativePropertyTraitRecord> propertyTraits,
        List<NativeFunctionTraitRecord> functionTraits
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
            functionTraits.Add(new NativeFunctionTraitRecord(
                owner, function.SourceName, GetNativeFunctionTraitBits(worldContext != null)
            ));
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
        if (scriptCallable && TryGetSingleDecimalDefault(property, out string value))
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
    private static ushort GetNativeDeclarationTraitBits(UhtProperty property) =>
        (ushort)((property.MetaData.ContainsKey("NativeConst") ? CppConstTrait : 0)
            | (property.RefQualifier != UhtPropertyRefQualifier.None ? CppRefTrait : 0)
            | (property is UhtByteProperty { Enum: not null } ? CppEnumAsByteTrait : 0));
    // === END CUVISS STUDIO EXTENSION: CS.Bridge.UHT.ScriptMethodReceiverTraits.Helper | by Cuviss Studio ===
    private static bool TryGetSingleDecimalDefault(
        UhtProperty property,
        out string value
    )
    {
        value = string.Empty;
        int tokenCount = 0;
        if (property.DefaultValueTokens == null)
        {
            return false;
        }
        foreach (UhtToken token in property.DefaultValueTokens)
        {
            tokenCount++;
            if (
                tokenCount > 1
                || token.TokenType.ToString() != "DecimalConst"
            )
            {
                return false;
            }
            value = token.Value.ToString();
        }
        if (
            tokenCount != 1
            || value.Length == 0
            || value.Any(character => character < ' ' || character > '~')
        )
        {
            value = string.Empty;
            return false;
        }
        return true;
    }
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
                .Append(record.TraitBits)
                .Append("U) },\n");
        }
    }
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
                character == '_' || char.IsAsciiLetterOrDigit(character)
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
        string Owner, string Function, ushort TraitBits
    ) : IKeyedRecord
    {
        public string SortKey => $"{Owner}\0{Function}";
    }
}
