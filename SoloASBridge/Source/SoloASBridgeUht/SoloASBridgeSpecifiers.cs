using System.Diagnostics.CodeAnalysis;
using EpicGames.UHT.Tables;
using EpicGames.UHT.Types;
using EpicGames.UHT.Utils;

namespace SoloASBridgeUht;

[UnrealHeaderTool]
[SuppressMessage(
    "CodeQuality",
    "IDE0051:Remove unused private members",
    Justification = "UHT discovers attributed handlers."
)]
public static class SoloASBridgeSpecifiers
{
    // The project file uses a source-built engine capability probe, not a UE version check.
#if !SOLOAS_UHT_BUILTIN_SCRIPT_CALLABLE
    [UhtSpecifier(
        Extends = UhtTableNames.Function,
        ValueType = UhtSpecifierValueType.None
    )]
    private static void ScriptCallableSpecifier(
        UhtSpecifierContext specifierContext
    )
    {
        specifierContext.MetaData.Add("ScriptCallable", "");
    }
#endif

#if !SOLOAS_UHT_BUILTIN_SCRIPT_READ_ONLY
    [UhtSpecifier(
        Extends = UhtTableNames.PropertyMember,
        ValueType = UhtSpecifierValueType.None
    )]
    private static void ScriptReadOnlySpecifier(
        UhtSpecifierContext specifierContext
    )
    {
        specifierContext.MetaData.Add("ScriptReadOnly", "");
    }
#endif

#if !SOLOAS_UHT_BUILTIN_SCRIPT_READ_WRITE
    [UhtSpecifier(
        Extends = UhtTableNames.PropertyMember,
        ValueType = UhtSpecifierValueType.None
    )]
    private static void ScriptReadWriteSpecifier(
        UhtSpecifierContext specifierContext
    )
    {
        specifierContext.MetaData.Add("ScriptReadWrite", "");
    }
#endif

#if !SOLOAS_UHT_BUILTIN_SCRIPT_EDIT_ONLY
    [UhtSpecifier(
        Extends = UhtTableNames.PropertyMember,
        ValueType = UhtSpecifierValueType.None
    )]
    private static void ScriptEditOnlySpecifier(
        UhtSpecifierContext specifierContext
    )
    {
        specifierContext.MetaData.Add("ScriptEditOnly", "");
    }
#endif
}
