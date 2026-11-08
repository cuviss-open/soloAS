using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace SoloASMcpHost;

internal sealed class RuntimeApiCatalog
{
    private const int MaximumProviderBytes = 67_108_864;
    private const int MaximumProviderUncompressedBytes = 268_435_456;
    private const int MaximumProviderSymbols = 250_000;
    private readonly object _lock = new();
    private readonly string _providerPath;
    private readonly List<ApiSnapshot> _history = new();
    private JsonObject? _database;
    private ProviderSnapshot? _providerSnapshot;
    private long _providerFileLength = -2;
    private long _providerWriteTimeTicks = -2;
    private string _providerStatus = "unavailable";

    public RuntimeApiCatalog(string providerPath) => _providerPath = providerPath;

    public void SetDatabase(JsonObject database)
    {
        lock (_lock)
        {
            _database = (JsonObject)database.DeepClone();
            RebuildSnapshotLocked();
        }
    }

    private void RefreshProviderSnapshot()
    {
        long fileLength = -1;
        long writeTimeTicks = 0;
        try
        {
            FileInfo info = new(_providerPath);
            if (info.Exists)
            {
                fileLength = info.Length;
                writeTimeTicks = info.LastWriteTimeUtc.Ticks;
            }
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or ArgumentException)
        {
            lock (_lock) _providerStatus = "unavailable:" + exception.GetType().Name;
            return;
        }

        lock (_lock)
        {
            if (fileLength == _providerFileLength && writeTimeTicks == _providerWriteTimeTicks) return;
        }

        ProviderSnapshot? snapshot = null;
        string status = fileLength < 0 ? "unavailable:missing" : "ready";
        if (fileLength >= 0)
        {
            if (fileLength > MaximumProviderBytes)
            {
                status = "invalid:oversized";
            }
            else
            {
                try
                {
                    snapshot = ReadProviderSnapshot(_providerPath);
                }
                catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException or FormatException or InvalidDataException)
                {
                    status = "invalid:" + exception.GetType().Name;
                }
            }
        }

        lock (_lock)
        {
            if (fileLength == _providerFileLength && writeTimeTicks == _providerWriteTimeTicks) return;
            _providerFileLength = fileLength;
            _providerWriteTimeTicks = writeTimeTicks;
            _providerSnapshot = snapshot;
            _providerStatus = status;
            RebuildSnapshotLocked();
        }
    }

    private static ProviderSnapshot ReadProviderSnapshot(string path)
    {
        using FileStream stream = new(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        using GZipStream gzip = new(stream, CompressionMode.Decompress, leaveOpen: true);
        using MemoryStream expanded = new();
        byte[] buffer = new byte[64 * 1024];
        long expandedBytes = 0;
        int read;
        while ((read = gzip.Read(buffer, 0, buffer.Length)) > 0)
        {
            expandedBytes += read;
            if (expandedBytes > MaximumProviderUncompressedBytes)
                throw new InvalidDataException("FApiCatalogProvider metadata projection exceeds the uncompressed byte limit");
            expanded.Write(buffer, 0, read);
        }
        expanded.Position = 0;
        using JsonDocument document = JsonDocument.Parse(expanded);
        JsonElement root = document.RootElement;
        if (root.ValueKind != JsonValueKind.Object
            || !root.TryGetProperty("schema", out JsonElement schema) || schema.ValueKind != JsonValueKind.String
            || schema.GetString() != "angelscript-auto-bind-provider-metadata-v1"
            || !root.TryGetProperty("snapshot_id", out JsonElement snapshotId) || snapshotId.ValueKind != JsonValueKind.String
            || !root.TryGetProperty("symbols", out JsonElement symbols) || symbols.ValueKind != JsonValueKind.Array)
            throw new InvalidDataException("FApiCatalogProvider metadata projection v1 envelope is incomplete");

        if (symbols.GetArrayLength() > MaximumProviderSymbols)
            throw new InvalidDataException("FApiCatalogProvider snapshot exceeds the symbol limit");
        string id = snapshotId.GetString() ?? string.Empty;
        if (id.Length == 0) throw new InvalidDataException("FApiCatalogProvider snapshot ID is empty");
        bool tiersEnabled = root.TryGetProperty("api_tiers_enabled", out JsonElement enabled) && enabled.ValueKind == JsonValueKind.True;
        List<ProviderSymbol> result = new(symbols.GetArrayLength());
        HashSet<string> ids = new(StringComparer.Ordinal);
        foreach (JsonElement symbol in symbols.EnumerateArray())
        {
            string symbolId = GetProviderString(symbol, "id");
            string origin = GetProviderString(symbol, "origin");
            string kind = GetProviderString(symbol, "kind");
            string name = GetProviderString(symbol, "name");
            if (symbolId.Length == 0 || kind.Length == 0 || name.Length == 0 || origin is not ("cpp" or "as") || !ids.Add(symbolId))
                throw new InvalidDataException("FApiCatalogProvider snapshot contains an invalid or duplicate symbol");
            string tier = GetProviderString(symbol, "tier", "core");
            if (tier is not ("core" or "extended" or "internal"))
                throw new InvalidDataException("FApiCatalogProvider snapshot contains an invalid tier");
            string[] tags = GetProviderStringArray(symbol, "tags");
            string sourceModule = GetProviderString(symbol, "source_module");
            string module = GetProviderString(symbol, "module");
            string owner = GetProviderString(symbol, "owner");
            string namespaceName = GetProviderString(symbol, "namespace");
            string declaration = GetProviderString(symbol, "declaration");
            string documentation = GetProviderString(symbol, "documentation_summary");
            bool available = GetProviderBool(symbol, "available", true);
            JsonElement metadata = JsonSerializer.SerializeToElement(new
            {
                origin,
                kind,
                module,
                owner,
                @namespace = namespaceName,
                name,
                declaration,
                available,
                tier,
                tags,
                source_module = sourceModule,
            });
            result.Add(new ProviderSymbol(symbolId, origin, kind,
                module, owner, namespaceName, name, declaration, documentation,
                available, tier, tags, sourceModule, metadata));
        }
        return new ProviderSnapshot(id, tiersEnabled, result.ToArray());
    }

    private void RebuildSnapshotLocked()
    {
        if (_database is null) return;
        List<ApiSymbol> symbols = MergeSymbols(Flatten(_database), _providerSnapshot);
        string snapshotContent = string.Join('\n', symbols.Select(SymbolCanonical).Order(StringComparer.Ordinal))
            + "\nprovider=" + (_providerSnapshot?.Id ?? string.Empty)
            + "\ntiers=" + (_providerSnapshot?.TiersEnabled == true ? "true" : "false");
        string id = Digest(snapshotContent).Replace("as-runtime-v1", "as-runtime-v2", StringComparison.Ordinal);
        ApiSnapshot snapshot = new(id, symbols.OrderBy(symbol => symbol.Id, StringComparer.Ordinal).ToArray(),
            DateTimeOffset.UtcNow, _providerSnapshot?.Id, _providerSnapshot?.TiersEnabled == true, _providerStatus);
        if (_history.Count > 0 && _history[^1].Id == snapshot.Id)
        {
            _history[^1] = snapshot;
            return;
        }
        _history.Add(snapshot);
        while (_history.Count > 3) _history.RemoveAt(0);
    }

    private static List<ApiSymbol> MergeSymbols(List<ApiSymbol> runtimeSymbols, ProviderSnapshot? provider)
    {
        if (provider is null) return runtimeSymbols;
        Dictionary<string, List<ProviderSymbol>> providerBySignature = new(StringComparer.Ordinal);
        foreach (ProviderSymbol symbol in provider.Symbols)
        {
            string key = SignatureKey(symbol.Kind, symbol.Name, symbol.Namespace, symbol.Owner,
                ProviderSignature(symbol));
            if (!providerBySignature.TryGetValue(key, out List<ProviderSymbol>? matches))
                providerBySignature[key] = matches = new List<ProviderSymbol>();
            matches.Add(symbol);
        }

        List<ApiSymbol> result = new(runtimeSymbols.Count);
        foreach (ApiSymbol runtime in runtimeSymbols)
        {
            string key = SignatureKey(runtime.Kind, runtime.Name, runtime.Namespace, runtime.Owner,
                NormalizeDeclarationSignature(runtime.Kind, runtime.Name, runtime.Declaration));
            ProviderSymbol[] matches = providerBySignature.TryGetValue(key, out List<ProviderSymbol>? values)
                ? values.ToArray()
                : Array.Empty<ProviderSymbol>();
            if (matches.Length == 1)
            {
                ProviderSymbol metadata = matches[0];
                result.Add(runtime with
                {
                    Origin = metadata.Origin,
                    Module = metadata.Module.Length > 0 ? metadata.Module : runtime.Module,
                    Owner = metadata.Owner.Length > 0 ? metadata.Owner : runtime.Owner,
                    Namespace = metadata.Namespace.Length > 0 ? metadata.Namespace : runtime.Namespace,
                    Documentation = metadata.Documentation.Length > 0 ? metadata.Documentation : runtime.Documentation,
                    Available = metadata.Available,
                    Tier = metadata.Tier,
                    Tags = metadata.Tags,
                    SourceModule = metadata.SourceModule,
                    MetadataAvailable = true,
                    ProviderMetadata = metadata.Metadata,
                });
            }
            else
            {
                result.Add(runtime);
            }
        }

        return result;
    }

    private static string ProviderSignature(ProviderSymbol symbol) =>
        NormalizeDeclarationSignature(symbol.Kind, symbol.Name, symbol.Declaration);

    private static string SignatureKey(string kind, string name, string namespaceName, string owner, string declaration)
    {
        string signature = kind is "type" or "enum" or "typedef" or "funcdef" or "enum_value"
            ? string.Empty
            : declaration;
        string qualifiedOwner = kind is "type" or "enum" or "typedef" or "funcdef" ? string.Empty : owner;
        return string.Join("|", kind, NormalizeTypeText(name), NormalizeTypeText(namespaceName),
            NormalizeTypeText(qualifiedOwner), signature);
    }

    private static string NormalizeDeclarationSignature(string kind, string name, string declaration)
    {
        string value = declaration.Trim();
        if (kind is "type" or "enum" or "typedef" or "funcdef" or "enum_value")
            return NormalizeTypeText(value);

        int open = value.IndexOf('(');
        int close = value.LastIndexOf(')');
        if (open >= 0 && close > open)
        {
            string arguments = value[(open + 1)..close];
            string suffix = value[(close + 1)..].Trim();
            string normalizedArguments = string.Join(",", SplitTopLevel(arguments, ',')
                .Select(NormalizeArgumentType));
            return "(" + normalizedArguments + ")" + (Regex.IsMatch(suffix, @"\bconst\b", RegexOptions.CultureInvariant) ? "const" : string.Empty);
        }

        if (kind is "property" or "global_property")
        {
            value = Regex.Replace(value, @"\s+" + Regex.Escape(name) + @"\s*$", string.Empty,
                RegexOptions.CultureInvariant);
        }
        return NormalizeTypeText(value);
    }

    private static IEnumerable<string> SplitTopLevel(string value, char separator)
    {
        if (value.Length == 0) yield break;
        int angle = 0;
        int round = 0;
        int square = 0;
        int start = 0;
        for (int index = 0; index < value.Length; ++index)
        {
            switch (value[index])
            {
                case '<': ++angle; break;
                case '>': if (angle > 0) --angle; break;
                case '(': ++round; break;
                case ')': if (round > 0) --round; break;
                case '[': ++square; break;
                case ']': if (square > 0) --square; break;
            }
            if (value[index] == separator && angle == 0 && round == 0 && square == 0)
            {
                yield return value[start..index].Trim();
                start = index + 1;
            }
        }
        yield return value[start..].Trim();
    }

    private static string NormalizeArgumentType(string argument)
    {
        string withoutDefault = SplitTopLevel(argument, '=')
            .FirstOrDefault() ?? string.Empty;
        string value = withoutDefault.Trim();
        int lastSpace = value.LastIndexOf(' ');
        if (lastSpace > 0 && Regex.IsMatch(value[(lastSpace + 1)..], @"^[A-Za-z_][A-Za-z_0-9]*$", RegexOptions.CultureInvariant))
            value = value[..lastSpace];
        return NormalizeTypeText(value);
    }

    private static string NormalizeTypeText(string value)
    {
        string normalized = Regex.Replace(value.Trim(), @"\s+", " ", RegexOptions.CultureInvariant);
        normalized = Regex.Replace(normalized, @"\s*([<>,*&\[\]])\s*", "$1", RegexOptions.CultureInvariant);
        normalized = Regex.Replace(normalized, @"\bint32\b", "int", RegexOptions.CultureInvariant);
        normalized = Regex.Replace(normalized, @"\buint32\b", "uint", RegexOptions.CultureInvariant);
        return normalized;
    }

    public object Call(string name, JsonElement arguments)
    {
        RefreshProviderSnapshot();
        ApiSnapshot snapshot;
        ApiSnapshot[] history;
        lock (_lock)
        {
            if (_history.Count == 0) throw new InvalidOperationException("DebugServer runtime type table is not available yet");
            snapshot = _history[^1];
            history = _history.ToArray();
        }

        return name switch
        {
            "as_api_search" => Search(arguments, snapshot, false),
            "as_api_list" => Search(arguments, snapshot, true),
            "as_api_get" => Get(arguments, snapshot, false),
            "as_api_explain" => Get(arguments, snapshot, true),
            "as_api_changes" => Changes(arguments, snapshot, history),
            "as_api_incremental" => Incremental(arguments, snapshot, history),
            _ => throw new ArgumentException("unknown API tool"),
        };
    }

    public static object[] ToolDefinitions() =>
    [
        Tool("as_api_search", "Search live DebugServer symbols with FApiCatalogProvider tier/tag/source metadata; default tier is core.", ["query", "text", "module", "origin", "kind", "tier", "tags", "exclude_tags", "offset", "limit"]),
        Tool("as_api_get", "Get a symbol by the ID returned from as_api_search, including tier and registration metadata.", ["id"], ["id"]),
        Tool("as_api_list", "List filtered API modules or symbols; view accepts modules, symbols, or types.", ["view", "query", "text", "module", "origin", "kind", "tier", "tags", "exclude_tags", "offset", "limit"]),
        Tool("as_api_explain", "Explain a symbol using its live runtime and FApiCatalogProvider evidence.", ["id"], ["id"]),
        Tool("as_api_incremental", "Get reset pages or deltas for the unified runtime and provider snapshot.", ["namespace", "cursor", "reset", "release", "offset", "limit"], ["namespace"]),
        Tool("as_api_changes", "Return changes from a recent unified API snapshot ID.", ["since_snapshot_id"], ["since_snapshot_id"]),
    ];

    private static object Search(JsonElement arguments, ApiSnapshot snapshot, bool allowView)
    {
        RequireAllowed(arguments, allowView
            ? ["view", "query", "text", "module", "origin", "kind", "tier", "tags", "exclude_tags", "offset", "limit"]
            : ["query", "text", "module", "origin", "kind", "tier", "tags", "exclude_tags", "offset", "limit"]);
        string view = GetString(arguments, "view", "modules");
        if (allowView && view is not ("modules" or "symbols" or "types")) throw new ArgumentException("view must be modules, symbols, or types");
        string query = GetString(arguments, "query", GetString(arguments, "text", string.Empty));
        string module = GetString(arguments, "module", string.Empty);
        string origin = GetString(arguments, "origin", string.Empty);
        string kind = GetString(arguments, "kind", string.Empty);
        string tier = GetString(arguments, "tier", "core").ToLowerInvariant();
        if (tier is not ("core" or "extended" or "internal" or "all"))
            throw new ArgumentException("tier must be core, extended, internal, or all");
        string[] requiredTags = GetStringArray(arguments, "tags");
        string[] excludedTags = GetStringArray(arguments, "exclude_tags");
        if (allowView && view == "types" && kind.Length == 0) kind = "type";
        int offset = GetInt(arguments, "offset", 0, int.MaxValue);
        int limit = GetInt(arguments, "limit", 100, 1000);
        ApiSymbol[] matches = snapshot.Symbols.Where(symbol =>
            (query.Length == 0 || SymbolText(symbol).Contains(query, StringComparison.OrdinalIgnoreCase))
            && (module.Length == 0 || symbol.Module.Equals(module, StringComparison.OrdinalIgnoreCase))
            && (origin.Length == 0 || symbol.Origin.Equals(origin, StringComparison.OrdinalIgnoreCase))
            && (kind.Length == 0 || symbol.Kind.Equals(kind, StringComparison.OrdinalIgnoreCase))
            && MatchesTierAndTags(snapshot, symbol, tier, requiredTags, excludedTags)).ToArray();
        if (allowView && view == "modules")
        {
            string[] modules = matches.Select(symbol => symbol.Module).Where(value => value.Length > 0)
                .Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal).ToArray();
            string[] registrationModules = matches.Select(symbol => symbol.SourceModule ?? string.Empty)
                .Where(value => value.Length > 0).Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal).ToArray();
            return new { snapshot = snapshot.ToJson(), modules, registration_modules = registrationModules,
                boundary = snapshot.MetadataSnapshotId is null
                    ? "The DebugServer type table has no registration-module metadata; no FApiCatalogProvider snapshot is available yet."
                    : "Modules come from FApiCatalogProvider symbols matched by runtime signature; unmatched DebugServer entries retain unknown metadata." };
        }
        return new { snapshot = snapshot.ToJson(), snapshot_id = snapshot.Id, total_matches = matches.Length,
            symbols = matches.Skip(offset).Take(limit).Select(symbol => symbol.ToJson()).ToArray() };
    }

    private static bool MatchesTierAndTags(ApiSnapshot snapshot, ApiSymbol symbol, string tier,
        string[] requiredTags, string[] excludedTags)
    {
        if (!snapshot.ApiTiersEnabled) return true;
        if (tier != "all" && !string.Equals(symbol.Tier, tier, StringComparison.OrdinalIgnoreCase)) return false;
        foreach (string required in requiredTags)
            if (!symbol.Tags.Contains(required, StringComparer.OrdinalIgnoreCase)) return false;
        foreach (string excluded in excludedTags)
            if (symbol.Tags.Contains(excluded, StringComparer.OrdinalIgnoreCase)) return false;
        return true;
    }

    private static object Get(JsonElement arguments, ApiSnapshot snapshot, bool explain)
    {
        RequireAllowed(arguments, ["id"]);
        string id = GetString(arguments, "id", string.Empty);
        if (id.Length == 0) throw new ArgumentException("id is required");
        ApiSymbol? symbol = snapshot.Symbols.FirstOrDefault(item => item.Id.Equals(id, StringComparison.Ordinal));
        if (symbol is null) throw new KeyNotFoundException("symbol was not present in the current runtime type table");
        return explain
            ? new { snapshot = snapshot.ToJson(), symbol = symbol.ToJson(), explanation = "Symbols are observed in the Editor DebugServer type table. FApiCatalogProvider contributes registration metadata only, joined by symbol signature." }
            : new { snapshot = snapshot.ToJson(), symbol = symbol.ToJson() };
    }

    private static object Changes(JsonElement arguments, ApiSnapshot snapshot, ApiSnapshot[] history)
    {
        RequireAllowed(arguments, ["since_snapshot_id"]);
        string previousId = GetString(arguments, "since_snapshot_id", string.Empty);
        if (previousId.Length == 0) throw new ArgumentException("since_snapshot_id is required");
        ApiSnapshot? previous = history.FirstOrDefault(item => item.Id == previousId);
        object delta = previous is null
            ? new { from_snapshot_id = previousId, to_snapshot_id = snapshot.Id, reset_required = true, reason = "baseline_unavailable", added = Array.Empty<object>(), removed = Array.Empty<object>(), changed = Array.Empty<object>() }
            : MakeDelta(previous, snapshot);
        return new { snapshot = snapshot.ToJson(), delta };
    }

    private static object Incremental(JsonElement arguments, ApiSnapshot snapshot, ApiSnapshot[] history)
    {
        RequireAllowed(arguments, ["namespace", "cursor", "reset", "release", "offset", "limit"]);
        string scope = GetString(arguments, "namespace", string.Empty);
        if (scope.Length == 0) throw new ArgumentException("namespace is required");
        bool release = GetBool(arguments, "release", false);
        if (release) return new { released = true };
        bool reset = GetBool(arguments, "reset", false);
        string cursor = GetString(arguments, "cursor", string.Empty);
        int offset = GetInt(arguments, "offset", 0, int.MaxValue);
        int limit = GetInt(arguments, "limit", 100, 1000);
        string currentCursor = "runtime:" + snapshot.Id;
        ApiSnapshot? previous = history.FirstOrDefault(item => "runtime:" + item.Id == cursor);
        if (!reset && previous is not null)
            return new { source = "debug_server_runtime_database", cursor = currentCursor, reset_required = false, reason = "runtime_snapshot_delta", delta = MakeDelta(previous, snapshot) };
        return new { source = "debug_server_runtime_database", cursor = currentCursor, reset_required = true,
            reason = reset || cursor.Length == 0 ? "reset_requested" : "cursor_unavailable",
            full_snapshot = new { snapshot = snapshot.ToJson(), snapshot_id = snapshot.Id, total_matches = snapshot.Symbols.Length,
                symbols = snapshot.Symbols.Skip(offset).Take(limit).Select(symbol => symbol.ToJson()).ToArray() },
            full_snapshot_paginated = snapshot.Symbols.Length > offset + limit };
    }

    private static object MakeDelta(ApiSnapshot before, ApiSnapshot after)
    {
        Dictionary<string, ApiSymbol> oldItems = before.Symbols.ToDictionary(item => item.Id, StringComparer.Ordinal);
        Dictionary<string, ApiSymbol> newItems = after.Symbols.ToDictionary(item => item.Id, StringComparer.Ordinal);
        ApiSymbol[] added = newItems.Where(pair => !oldItems.ContainsKey(pair.Key)).Select(pair => pair.Value).ToArray();
        ApiSymbol[] removed = oldItems.Where(pair => !newItems.ContainsKey(pair.Key)).Select(pair => pair.Value).ToArray();
        ApiSymbol[] changed = newItems.Where(pair => oldItems.TryGetValue(pair.Key, out ApiSymbol? oldItem)
            && SymbolCanonical(oldItem) != SymbolCanonical(pair.Value)).Select(pair => pair.Value).ToArray();
        return new { from_snapshot_id = before.Id, to_snapshot_id = after.Id, reset_required = false,
            reason = "runtime_snapshot_diff", added = added.Select(item => item.ToJson()).ToArray(),
            removed = removed.Select(item => item.ToJson()).ToArray(), changed = changed.Select(item => item.ToJson()).ToArray(),
            counts = new { added = added.Length, removed = removed.Length, changed = changed.Length } };
    }

    private static List<ApiSymbol> Flatten(JsonObject database)
    {
        List<ApiSymbol> result = new();
        foreach ((string key, JsonNode? rawNode) in database)
        {
            if (rawNode is not JsonObject descriptor) continue;
            string qualifiedOwner = key.StartsWith("__", StringComparison.Ordinal) ? string.Empty : key;
            string namespaceName = key.StartsWith("__", StringComparison.Ordinal) ? key[2..] : string.Empty;
            string owner = GetNodeFieldString(descriptor, "owner", qualifiedOwner);
            string descriptorNamespace = GetNodeFieldString(descriptor, "namespace", namespaceName);
            string descriptorName = GetNodeFieldString(descriptor, "name", key);
            string descriptorModule = GetNodeFieldString(descriptor, "module");
            string descriptorOrigin = GetNodeFieldString(descriptor, "origin", "runtime");
            bool isNamespace = key.StartsWith("__", StringComparison.Ordinal);
            string typeDocumentation = GetNodeFieldString(descriptor, "doc");
            if (!isNamespace && !key.StartsWith('_'))
            {
                result.Add(MakeSymbol("type", descriptorName, descriptorNamespace, owner,
                    GetNodeFieldString(descriptor, "declaration", descriptorName), typeDocumentation, true, new { },
                    descriptorModule, descriptorOrigin));
            }
            if (descriptor["properties"] is JsonObject properties)
            {
                bool isEnum = GetNodeBool(descriptor, "isEnum");
                if (isNamespace && isEnum && namespaceName.Length > 0)
                    result.Add(MakeSymbol("enum", namespaceName, namespaceName, string.Empty,
                        "enum " + namespaceName, typeDocumentation, true, new { }, descriptorModule, descriptorOrigin));
                foreach ((string name, JsonNode? propertyNode) in properties)
                {
                    if (propertyNode is not JsonArray property || property.Count == 0) continue;
                    string declaration = property[0]?.GetValue<string>() ?? string.Empty;
                    if (isEnum)
                    {
                        result.Add(MakeSymbol("enum_value", name, descriptorNamespace, owner, descriptorName,
                            property.Count > 1 ? GetNodeString(property[1]) : string.Empty, true,
                            new { enum_type = declaration }, descriptorModule, descriptorOrigin));
                    }
                    else
                    {
                        string doc = property.Skip(1).Select(node => GetNodeString(node)).FirstOrDefault(value => value.Length > 0) ?? string.Empty;
                        int flags = property.Skip(1).Select(GetNodeInt).FirstOrDefault(value => value != 0);
                        result.Add(MakeSymbol(isNamespace ? "global_property" : "property", name,
                            isNamespace ? namespaceName : descriptorNamespace, owner, declaration, doc,
                            (flags & 4) == 0, new { runtime_flags = flags }, descriptorModule, descriptorOrigin));
                    }
                }
            }
            if (descriptor["methods"] is JsonArray methods)
            {
                foreach (JsonNode? methodNode in methods)
                {
                    if (methodNode is not JsonObject method) continue;
                    string name = GetNodeFieldString(method, "name");
                    if (name.Length == 0) continue;
                    string returnType = GetNodeFieldString(method, "return", "void");
                    string args = method["args"] is JsonArray argList
                        ? string.Join(", ", argList.OfType<JsonObject>().Select(FormatArgument))
                        : string.Empty;
                    string declaration = $"{returnType} {name}({args})";
                    if (GetNodeBool(method, "const")) declaration += " const";
                    bool available = !GetNodeBool(method, "protected") && GetNodeBool(method, "callable", true);
                    bool constructor = GetNodeBool(method, "isConstructor");
                    string kind = constructor ? "factory" : isNamespace ? "global_function" : "method";
                    result.Add(MakeSymbol(kind, name, isNamespace ? namespaceName : descriptorNamespace,
                        owner, declaration, GetNodeFieldString(method, "doc"), available,
                        new { is_property = GetNodeBool(method, "isProperty"), is_constructor = constructor },
                        descriptorModule, descriptorOrigin));
                }
            }
        }
        return result.GroupBy(item => item.Id, StringComparer.Ordinal).Select(group => group.First())
            .OrderBy(item => item.Id, StringComparer.Ordinal).ToList();
    }

    private static string FormatArgument(JsonObject argument)
    {
        string type = GetNodeFieldString(argument, "type", "?");
        string name = GetNodeFieldString(argument, "name");
        string defaultValue = GetNodeFieldString(argument, "default");
        string value = name.Length == 0 ? type : type + " " + name;
        return defaultValue.Length == 0 ? value : value + " = " + defaultValue;
    }

    private static ApiSymbol MakeSymbol(string kind, string name, string namespaceName, string owner,
        string declaration, string documentation, bool available, object extra,
        string module = "", string origin = "runtime")
    {
        string canonical = string.Join("|", kind, name, namespaceName, owner, declaration,
            available ? "true" : "false", JsonSerializer.Serialize(extra));
        if (origin == "as" && module.Length > 0)
            canonical += "|origin=as|module=" + module;
        string id = Digest(canonical);
        return new ApiSymbol(id, origin, kind, module, owner, namespaceName, name, declaration,
            documentation, available, string.Empty, Array.Empty<string>(), string.Empty, false,
            JsonSerializer.SerializeToElement(extra), JsonSerializer.SerializeToElement(new { }));
    }

    private static string SymbolText(ApiSymbol symbol) =>
        string.Join(" ", symbol.Id, symbol.Origin, symbol.Module, symbol.Owner, symbol.Namespace, symbol.Name,
            symbol.Declaration, symbol.Documentation, symbol.Tier, string.Join(" ", symbol.Tags), symbol.SourceModule);

    private static string SymbolCanonical(ApiSymbol symbol) =>
        string.Join("|", symbol.Id, symbol.Origin, symbol.Kind, symbol.Module, symbol.Owner, symbol.Namespace, symbol.Name,
            symbol.Declaration, symbol.Documentation, symbol.Available ? "true" : "false", symbol.Tier,
            string.Join(",", symbol.Tags), symbol.SourceModule, symbol.MetadataAvailable ? "true" : "false",
            symbol.Extra.GetRawText(), symbol.ProviderMetadata.GetRawText());

    private static string Digest(string value) => "as-runtime-v1:sha1=" + Convert.ToHexString(SHA1.HashData(Encoding.UTF8.GetBytes(value))).ToLowerInvariant();

    private static object Tool(string name, string description, string[] properties, string[]? required = null)
    {
        var schemaProperties = properties.ToDictionary(property => property, _ => (object)new { type = "string" }, StringComparer.Ordinal);
        foreach (string property in new[] { "offset", "limit" })
            if (schemaProperties.ContainsKey(property)) schemaProperties[property] = new { type = "integer", minimum = 0 };
        foreach (string property in new[] { "reset", "release" })
            if (schemaProperties.ContainsKey(property)) schemaProperties[property] = new { type = "boolean" };
        foreach (string property in new[] { "tags", "exclude_tags" })
            if (schemaProperties.ContainsKey(property)) schemaProperties[property] = new { type = "array", items = new { type = "string", minLength = 1 }, uniqueItems = true };
        if (schemaProperties.ContainsKey("tier"))
            schemaProperties["tier"] = new { type = "string", @enum = new[] { "core", "extended", "internal", "all" }, @default = "core" };
        return new { name, description, inputSchema = new { type = "object", properties = schemaProperties,
            required = required ?? Array.Empty<string>(), additionalProperties = false } };
    }

    private static string GetString(JsonElement json, string name, string fallback)
    {
        if (!json.TryGetProperty(name, out JsonElement value)) return fallback;
        if (value.ValueKind != JsonValueKind.String) throw new ArgumentException($"{name} must be a string");
        return value.GetString() ?? fallback;
    }

    private static string[] GetStringArray(JsonElement json, string name)
    {
        if (!json.TryGetProperty(name, out JsonElement value)) return Array.Empty<string>();
        if (value.ValueKind != JsonValueKind.Array) throw new ArgumentException($"{name} must be an array of strings");
        List<string> result = new();
        HashSet<string> seen = new(StringComparer.OrdinalIgnoreCase);
        foreach (JsonElement item in value.EnumerateArray())
        {
            if (item.ValueKind != JsonValueKind.String || string.IsNullOrWhiteSpace(item.GetString()))
                throw new ArgumentException($"{name} must contain only non-empty strings");
            string tag = item.GetString()!;
            if (!seen.Add(tag)) throw new ArgumentException($"{name} must not contain duplicate values");
            result.Add(tag);
            if (result.Count > 64) throw new ArgumentException($"{name} must contain at most 64 values");
        }
        return result.ToArray();
    }

    private static string GetProviderString(JsonElement symbol, string name, string fallback = "") =>
        symbol.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? fallback : fallback;

    private static string[] GetProviderStringArray(JsonElement symbol, string name)
    {
        if (!symbol.TryGetProperty(name, out JsonElement value)) return Array.Empty<string>();
        if (value.ValueKind != JsonValueKind.Array) throw new InvalidDataException($"{name} must be an array");
        List<string> result = new();
        HashSet<string> seen = new(StringComparer.OrdinalIgnoreCase);
        foreach (JsonElement item in value.EnumerateArray())
        {
            if (item.ValueKind != JsonValueKind.String || string.IsNullOrWhiteSpace(item.GetString()))
                throw new InvalidDataException($"{name} must contain non-empty strings");
            string tag = item.GetString()!;
            if (!seen.Add(tag)) throw new InvalidDataException($"{name} contains duplicate values");
            result.Add(tag);
        }
        return result.ToArray();
    }

    private static bool GetProviderBool(JsonElement symbol, string name, bool fallback) =>
        symbol.TryGetProperty(name, out JsonElement value) && value.ValueKind is JsonValueKind.True or JsonValueKind.False
            ? value.GetBoolean() : fallback;

    private static int GetInt(JsonElement json, string name, int fallback, int max)
    {
        if (!json.TryGetProperty(name, out JsonElement value)) return fallback;
        if (!value.TryGetInt32(out int result) || result < 0 || result > max) throw new ArgumentException($"{name} must be an integer from 0 to {max}");
        return result;
    }

    private static bool GetBool(JsonElement json, string name, bool fallback)
    {
        if (!json.TryGetProperty(name, out JsonElement value)) return fallback;
        if (value.ValueKind is not (JsonValueKind.True or JsonValueKind.False)) throw new ArgumentException($"{name} must be a boolean");
        return value.GetBoolean();
    }

    private static void RequireAllowed(JsonElement arguments, string[] allowed)
    {
        if (arguments.ValueKind is JsonValueKind.Undefined or JsonValueKind.Null) return;
        if (arguments.ValueKind != JsonValueKind.Object) throw new ArgumentException("arguments must be an object");
        foreach (JsonProperty property in arguments.EnumerateObject())
            if (!allowed.Contains(property.Name, StringComparer.Ordinal)) throw new ArgumentException("unknown argument: " + property.Name);
    }

    private static string GetNodeFieldString(JsonNode? node, string field, string fallback = "") =>
        GetNodeScalarString(node is JsonObject obj ? obj[field] : null, fallback);

    private static string GetNodeString(JsonNode? node) => GetNodeScalarString(node, string.Empty);

    private static string GetNodeScalarString(JsonNode? node, string fallback)
    {
        try { return node?.GetValue<string>() ?? fallback; }
        catch (InvalidOperationException) { return fallback; }
    }
    private static int GetNodeInt(JsonNode? node)
    {
        try { return node?.GetValue<int>() ?? 0; }
        catch (InvalidOperationException) { return 0; }
    }
    private static bool GetNodeBool(JsonObject obj, string field, bool fallback = false)
    {
        try { return obj[field]?.GetValue<bool>() ?? fallback; }
        catch (InvalidOperationException) { return fallback; }
    }

    private sealed record ProviderSnapshot(string Id, bool TiersEnabled, ProviderSymbol[] Symbols);

    private sealed record ProviderSymbol(string Id, string Origin, string Kind, string Module, string Owner,
        string Namespace, string Name, string Declaration, string Documentation, bool Available,
        string Tier, string[] Tags, string SourceModule, JsonElement Metadata);

    private sealed record ApiSnapshot(string Id, ApiSymbol[] Symbols, DateTimeOffset CapturedAt,
        string? MetadataSnapshotId, bool ApiTiersEnabled, string ProviderStatus)
    {
        public object ToJson() => new { id = Id, ready = true, source = "debug_server_runtime_database",
            captured_at_utc = CapturedAt, metadata_source = "FApiCatalogProvider", metadata_snapshot_id = MetadataSnapshotId,
            api_tiers_enabled = ApiTiersEnabled, metadata_status = ProviderStatus };
    }

    private sealed record ApiSymbol(string Id, string Origin, string Kind, string Module, string Owner,
        string Namespace, string Name, string Declaration, string Documentation, bool Available, string Tier,
        string[] Tags, string SourceModule, bool MetadataAvailable, JsonElement Extra, JsonElement ProviderMetadata)
    {
        public object ToJson() => new { id = Id, origin = Origin, kind = Kind, module = Module, owner = Owner,
            @namespace = Namespace, name = Name, declaration = Declaration, documentation = Documentation,
            available = Available, tier = Tier.Length > 0 ? Tier : "unknown", tags = Tags,
            source_module = SourceModule.Length > 0 ? SourceModule : null,
            tier_metadata_available = MetadataAvailable, runtime_metadata = Extra,
            registration_metadata = MetadataAvailable ? ProviderMetadata : (JsonElement?)null };
    }
}
