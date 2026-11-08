// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
using System.Text.Json;
using System.Text.Json.Nodes;

namespace SoloASMcpHost;

internal sealed class ProjectInfoProvider
{
    private readonly string _projectFile;
    private readonly string _engineRoot;
    private readonly string _lane;

    public ProjectInfoProvider(string projectFile, string engineRoot, string lane)
    {
        _projectFile = Path.GetFullPath(projectFile);
        _engineRoot = Path.GetFullPath(engineRoot);
        _lane = lane;
    }

    public object Get(DebugServerClient debug)
    {
        if (!File.Exists(_projectFile)) throw new FileNotFoundException("project file was not found", _projectFile);
        using JsonDocument project = JsonDocument.Parse(File.ReadAllBytes(_projectFile), new JsonDocumentOptions { MaxDepth = 64 });
        JsonElement root = project.RootElement;
        string projectRoot = Path.GetDirectoryName(_projectFile)!;
        string projectName = Path.GetFileNameWithoutExtension(_projectFile);
        string association = GetString(root, "EngineAssociation", string.Empty);
        List<JsonElement> pluginEntries = GetArray(root, "Plugins").ToList();
        List<PluginInfo> plugins = new();
        foreach (JsonElement entry in pluginEntries)
        {
            string name = GetString(entry, "Name", string.Empty);
            if (name.Length == 0) continue;
            bool enabled = GetBool(entry, "Enabled", true);
            string? descriptorPath = FindPluginDescriptor(projectRoot, name);
            plugins.Add(new PluginInfo(name, enabled, GetBool(entry, "Optional", false), descriptorPath,
                descriptorPath is null ? Array.Empty<ModuleInfo>() : ReadPluginModules(descriptorPath)));
        }

        List<ModuleInfo> projectModules = GetArray(root, "Modules")
            .Select(module => MakeModule(GetString(module, "Name", string.Empty), module))
            .Where(module => module.Name.Length > 0).ToList();
        string[] scriptRoots = FindScriptRoots(projectRoot, plugins);
        IniSection[] config = ReadAngelscriptIni(projectRoot, plugins);
        string[] targetRoots = { Path.Combine(projectRoot, "Source"), Path.Combine(projectRoot, "Intermediate", "Source") };
        string[] targets = targetRoots.Where(Directory.Exists)
            .SelectMany(path => Directory.EnumerateFiles(path, "*.Target.cs", SearchOption.AllDirectories))
            .Select(Path.GetFullPath).Distinct(StringComparer.OrdinalIgnoreCase).Order(StringComparer.Ordinal).ToArray();
        string[] maps = Directory.Exists(Path.Combine(projectRoot, "Content"))
            ? Directory.EnumerateFiles(Path.Combine(projectRoot, "Content"), "*.umap", SearchOption.AllDirectories)
                .Select(path => Path.GetRelativePath(projectRoot, path).Replace('\\', '/')).Order(StringComparer.Ordinal).ToArray()
            : Array.Empty<string>();

        DiagnosticRecord[] diagnostics = debug.Diagnostics.ToArray();
        int errors = diagnostics.Count(item => item.IsError);
        int warnings = diagnostics.Count(item => !item.IsError && !item.IsInfo);
        string? runtimeLane = ReadRuntimeLane(debug.Settings);
        string compileState = !debug.IsConnected ? "unavailable"
            : errors > 0 ? "errors_reported"
            : warnings > 0 ? "warnings_reported"
            : debug.IsDatabaseReady ? "no_errors_reported" : "unknown";

        return new
        {
            project = new { name = projectName, file = _projectFile, root = projectRoot },
            engine = new { version = ReadEngineVersion(), association, lane = _lane, runtimeLane },
            plugins = plugins.Select(plugin => new { plugin.Name, plugin.Enabled, plugin.Optional, plugin.Descriptor,
                modules = plugin.Modules.Select(module => module.ToJson()).ToArray() }).ToArray(),
            modules = projectModules.Select(module => module.ToJson()).ToArray(),
            angelScript = new
            {
                scriptRoots,
                iniSettings = config.Select(item => new { file = item.File, section = item.Section, values = item.Values }).ToArray(),
            },
            buildTargets = targets,
            maps,
            compileStatus = new { state = compileState, evidence = debug.IsDatabaseReady ? "debug_server_type_table_and_current_diagnostics" : "debug_server_connection",
                isCompiling = (bool?)null, errors, warnings, diagnostics = diagnostics.Length },
            live = new { debugServerConnected = debug.IsConnected, typeTableReady = debug.IsDatabaseReady,
                debugServerVersion = debug.ServerVersion, debugSettings = debug.Settings },
        };
    }

    private string ReadEngineVersion()
    {
        string versionFile = Path.Combine(_engineRoot, "Engine", "Build", "Build.version");
        if (!File.Exists(versionFile)) return "unknown";
        try
        {
            using JsonDocument version = JsonDocument.Parse(File.ReadAllBytes(versionFile));
            JsonElement root = version.RootElement;
            return $"{GetInt(root, "MajorVersion")}.{GetInt(root, "MinorVersion")}.{GetInt(root, "PatchVersion")}";
        }
        catch (Exception exception) when (exception is IOException or JsonException) { return "unknown"; }
    }

    private string? FindPluginDescriptor(string projectRoot, string name)
    {
        string projectPlugins = Path.Combine(projectRoot, "Plugins");
        if (Directory.Exists(projectPlugins))
        {
            string? match = Directory.EnumerateFiles(projectPlugins, name + ".uplugin", SearchOption.AllDirectories).FirstOrDefault();
            if (match is not null) return match;
        }
        string enginePlugins = Path.Combine(_engineRoot, "Engine", "Plugins");
        if (Directory.Exists(enginePlugins))
        {
            string? match = Directory.EnumerateFiles(enginePlugins, name + ".uplugin", SearchOption.AllDirectories).FirstOrDefault();
            if (match is not null) return match;
        }
        return null;
    }

    private static ModuleInfo[] ReadPluginModules(string descriptorPath)
    {
        try
        {
            using JsonDocument descriptor = JsonDocument.Parse(File.ReadAllBytes(descriptorPath));
            return GetArray(descriptor.RootElement, "Modules").Select(module => MakeModule(GetString(module, "Name", string.Empty), module))
                .Where(module => module.Name.Length > 0).ToArray();
        }
        catch (Exception exception) when (exception is IOException or JsonException) { return Array.Empty<ModuleInfo>(); }
    }

    private static string[] FindScriptRoots(string projectRoot, IReadOnlyList<PluginInfo> plugins)
    {
        List<string> roots = new();
        string projectScript = Path.Combine(projectRoot, "Script");
        roots.Add(Path.GetFullPath(projectScript));
        foreach (PluginInfo plugin in plugins.Where(item => item.Enabled && item.Descriptor is not null))
        {
            string script = Path.Combine(Path.GetDirectoryName(plugin.Descriptor!)!, "Script");
            if (Directory.Exists(script)) roots.Add(Path.GetFullPath(script));
        }
        return roots.Distinct(StringComparer.OrdinalIgnoreCase).Order(StringComparer.Ordinal).ToArray();
    }

    private IniSection[] ReadAngelscriptIni(string projectRoot, IReadOnlyList<PluginInfo> plugins)
    {
        IEnumerable<string> roots = new[] { Path.Combine(projectRoot, "Config"), Path.Combine(_engineRoot, "Engine", "Config") }
            .Concat(plugins.Where(item => item.Enabled && item.Descriptor is not null)
                .Select(item => Path.Combine(Path.GetDirectoryName(item.Descriptor!)!, "Config")));
        List<IniSection> result = new();
        foreach (string root in roots.Where(Directory.Exists))
        {
            foreach (string path in Directory.EnumerateFiles(root, "*.ini", SearchOption.AllDirectories).Order(StringComparer.Ordinal))
            {
                string? section = null;
                List<string> lines = new();
                foreach (string raw in File.ReadLines(path))
                {
                    string line = raw.Trim();
                    if (line.StartsWith("[", StringComparison.Ordinal) && line.EndsWith("]", StringComparison.Ordinal))
                    {
                        if (section is not null && IsAngelscriptConfig(section) && lines.Count > 0)
                            result.Add(new IniSection(Path.GetFullPath(path), section, lines.ToArray()));
                        section = line[1..^1];
                        lines = new List<string>();
                    }
                    else if (section is not null && IsAngelscriptConfig(section) && line.Length > 0 && !line.StartsWith(';')) lines.Add(line);
                }
                if (section is not null && IsAngelscriptConfig(section) && lines.Count > 0)
                    result.Add(new IniSection(Path.GetFullPath(path), section, lines.ToArray()));
            }
        }
        return result.ToArray();
    }

    private static bool IsAngelscriptConfig(string section) => section.Contains("Angelscript", StringComparison.OrdinalIgnoreCase)
        && !section.Contains("AutoBind", StringComparison.OrdinalIgnoreCase)
        && !section.Contains("ModKit", StringComparison.OrdinalIgnoreCase);

    private static string? ReadRuntimeLane(JsonObject settings)
    {
        return settings["useAngelscriptHaze"] is JsonValue value && value.TryGetValue(out bool useHaze)
            ? useHaze ? "haze" : "stock"
            : null;
    }

    private static ModuleInfo MakeModule(string name, JsonElement element)
    {
        string moduleType = GetString(element, "Type", string.Empty);
        string loadingPhase = GetString(element, "LoadingPhase", string.Empty);
        string moduleTier = GetString(element, "ModuleTier", string.Empty);
        string[] tags = element.TryGetProperty("Tags", out JsonElement tagsElement) && tagsElement.ValueKind == JsonValueKind.Array
            ? tagsElement.EnumerateArray().Where(item => item.ValueKind == JsonValueKind.String).Select(item => item.GetString()!).ToArray()
            : Array.Empty<string>();
        return new ModuleInfo(name, moduleType, loadingPhase, moduleTier, tags);
    }

    private static IEnumerable<JsonElement> GetArray(JsonElement element, string field) =>
        element.TryGetProperty(field, out JsonElement value) && value.ValueKind == JsonValueKind.Array ? value.EnumerateArray().ToArray() : Array.Empty<JsonElement>();

    private static string GetString(JsonElement element, string field, string fallback) =>
        element.TryGetProperty(field, out JsonElement value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? fallback : fallback;

    private static bool GetBool(JsonElement element, string field, bool fallback) =>
        element.TryGetProperty(field, out JsonElement value) && value.ValueKind is JsonValueKind.True or JsonValueKind.False ? value.GetBoolean() : fallback;

    private static int GetInt(JsonElement element, string field) => element.TryGetProperty(field, out JsonElement value) && value.TryGetInt32(out int number) ? number : 0;

    private sealed record PluginInfo(string Name, bool Enabled, bool Optional, string? Descriptor, ModuleInfo[] Modules);
    private sealed record IniSection(string File, string Section, string[] Values);
    private sealed record ModuleInfo(string Name, string Type, string LoadingPhase, string ModuleTier, string[] Tags)
    {
        public object ToJson() => new { name = Name, type = Type, loadingPhase = LoadingPhase,
            moduleTier = ModuleTier.Length > 0 ? ModuleTier : null, tags = Tags };
    }
}
