// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace SoloASMcpHost;

internal sealed class McpHostService : IAsyncDisposable
{
    private readonly HostOptions _options;
    private readonly DebugServerClient _debug;
    private readonly EditClient _edit;
    private readonly RuntimeApiCatalog _catalog;
    private readonly ExternalTools _external;
    private readonly ProjectInfoProvider _projectInfo;
    private static readonly UTF8Encoding Utf8WithoutBom = new(false, true);

    public McpHostService(HostOptions options)
    {
        _options = options;
        string projectRoot = Path.GetDirectoryName(Path.GetFullPath(options.ProjectPath))!;
        string snapshotRoot = Path.Combine(projectRoot, "Saved", "SoloASDeveloperTools", "Mcp");
        string snapshotPath = Path.Combine(snapshotRoot, "debug-database-v1.json");
        _external = new ExternalTools(options);
        string metadataRoot = Path.Combine(projectRoot, "Saved", "AngelscriptAutoBind", "Mcp");
        _catalog = new RuntimeApiCatalog(Path.Combine(metadataRoot, "runtime-api-metadata-v1.json.gz"));
        _debug = new DebugServerClient(options.DebugHost, options.DebugPort, snapshotPath,
            Path.Combine(metadataRoot, "debug-database-v1.json"));
        _edit = new EditClient(projectRoot, options.DebugHost, options.DebugPort, _debug);
        string engineRoot = ResolveEngineRoot(options.EnginePath);
        string lane = options.Lane.Length > 0 ? options.Lane : engineRoot.Contains("haze", StringComparison.OrdinalIgnoreCase) ? "haze" : "stock";
        _projectInfo = new ProjectInfoProvider(options.ProjectPath, engineRoot, lane);
    }

    public async Task<byte[]> HandleAsync(byte[] requestBytes)
    {
        using JsonDocument document = JsonDocument.Parse(requestBytes);
        JsonElement root = document.RootElement;
        JsonElement id = JsonSerializer.SerializeToElement<object?>(null);
        if (root.ValueKind != JsonValueKind.Object) return RpcError(id, -32600, "invalid request");
        if (root.TryGetProperty("id", out JsonElement requestId)) id = requestId.Clone();
        if (root.EnumerateObject().GroupBy(p => p.Name).Any(g => g.Count() > 1)
            || !root.TryGetProperty("jsonrpc", out JsonElement version) || version.ValueKind != JsonValueKind.String || version.GetString() != "2.0"
            || !root.TryGetProperty("method", out JsonElement methodValue) || methodValue.ValueKind != JsonValueKind.String || string.IsNullOrEmpty(methodValue.GetString())
            || (root.TryGetProperty("params", out JsonElement badParams) && badParams.ValueKind != JsonValueKind.Object))
            return RpcError(id, -32600, "invalid request");
        string method = methodValue.GetString()!;
        if (!root.TryGetProperty("id", out _)) return Array.Empty<byte>();
        JsonElement parameters = root.TryGetProperty("params", out JsonElement paramValue) ? paramValue : JsonSerializer.SerializeToElement(new { });

        try
        {
            object result = method switch
            {
                "initialize" => MakeInitializeResult(),
                "ping" => new { },
                "tools/list" => await MakeToolsListAsync().ConfigureAwait(false),
                "tools/call" => await InvokeToolAsync(parameters).ConfigureAwait(false),
                _ => throw new MissingMethodException(method),
            };
            return RpcResult(id, result);
        }
        catch (MissingMethodException)
        {
            return RpcError(id, -32601, "method not found");
        }
        catch (OperationCanceledException)
        {
            return method == "tools/call" ? RpcResult(id, ToolResult(new { error = "request_timed_out" }, true))
                : RpcError(id, -32000, "request_timed_out");
        }
        catch (Exception exception) when (exception is JsonException or InvalidOperationException or ArgumentException or KeyNotFoundException or IOException or SocketException or TimeoutException or System.ComponentModel.Win32Exception or UnauthorizedAccessException)
        {
            if (method == "tools/call") return RpcResult(id, ToolResult(new { error = exception.Message }, true));
            return RpcError(id, -32602, exception.Message);
        }
    }

    private async Task<object> InvokeToolAsync(JsonElement parameters)
    {
        RequireAllowed(parameters, "name", "arguments");
        if (parameters.ValueKind != JsonValueKind.Object || !parameters.TryGetProperty("name", out JsonElement nameValue)
            || nameValue.ValueKind != JsonValueKind.String) throw new ArgumentException("tools/call requires a string name");
        string name = nameValue.GetString() ?? string.Empty;
        JsonElement arguments = parameters.TryGetProperty("arguments", out JsonElement argumentValue)
            ? argumentValue : JsonDocument.Parse("{}").RootElement.Clone();
        object result;
        using CancellationTokenSource timeout = new(TimeSpan.FromMilliseconds(_options.TimeoutMilliseconds));
        CancellationToken token = timeout.Token;

        if (name.StartsWith("as_api_", StringComparison.Ordinal))
        {
            JsonObject? database;
            if (_debug.IsConnected && !_debug.IsDatabaseReady)
                database = await _debug.WaitForDatabaseAsync(Math.Min(10_000, _options.TimeoutMilliseconds)).ConfigureAwait(false);
            else
            {
                database = _debug.Database;
                if (database is null && !_debug.IsConnected)
                {
                    _ = await _debug.WaitForConnectionAsync(Math.Min(5000, _options.TimeoutMilliseconds)).ConfigureAwait(false);
                    database = _debug.IsConnected
                        ? await _debug.WaitForDatabaseAsync(Math.Min(10_000, _options.TimeoutMilliseconds)).ConfigureAwait(false)
                        : _debug.Database;
                }
            }
            if (database is not null) _catalog.SetDatabase(database);
            result = _catalog.Call(name, arguments);
        }
        else if (name.StartsWith("soloas_package_", StringComparison.Ordinal))
        {
            JsonElement envelope = await _external.PackageAsync(name, arguments, token).ConfigureAwait(false);
            return ToolResult(envelope, !envelope.GetProperty("ok").GetBoolean());
        }
        else if (name.Contains('.', StringComparison.Ordinal))
        {
            RequireObject(arguments);
            JsonElement envelope = await _external.ProviderAsync(new
            {
                schemaVersion = 1, operation = "mcp.call", providerId = name.Split('.')[0],
                name, jobId = Guid.NewGuid().ToString("N"), arguments,
            }, token).ConfigureAwait(false);
            return ToolResult(envelope, !envelope.GetProperty("ok").GetBoolean());
        }
        else if (name == "as_edit_begin")
        {
            RequireAllowed(arguments, "timeoutSeconds");
            int seconds = arguments.TryGetProperty("timeoutSeconds", out _) ? RequiredInt(arguments, "timeoutSeconds", 1, 600) : 120;
            result = await _edit.CallAsync("begin", new { timeoutSeconds = seconds }, token).ConfigureAwait(false);
        }
        else if (name == "as_edit_end")
        {
            RequireAllowed(arguments, "leaseId");
            result = await _edit.CallAsync("end", new { leaseId = RequiredString(arguments, "leaseId") }, token).ConfigureAwait(false);
        }
        else if (name == "as_compile")
        {
            RequireNoArguments(arguments);
            result = await _edit.CallAsync("compile", null, token).ConfigureAwait(false);
        }
        else if (name == "as_diagnostics")
        {
            RequireNoArguments(arguments);
            if (!_debug.IsConnected)
                _ = await _debug.WaitForConnectionAsync(Math.Min(2000, _options.TimeoutMilliseconds)).WaitAsync(token).ConfigureAwait(false);
            _ = await _debug.WaitForDatabaseAsync(Math.Min(5000, _options.TimeoutMilliseconds)).WaitAsync(token).ConfigureAwait(false);
            await Task.Delay(250, token).ConfigureAwait(false);
            DiagnosticRecord[] diagnostics = _debug.Diagnostics.ToArray();
            JsonElement? snapshot = _edit.Snapshot;
            result = new { connected = _debug.IsConnected, source = "debug_server",
                generation = snapshot?.GetProperty("generation").GetInt64(),
                state = snapshot?.GetProperty("state").GetString(),
                editError = _edit.LastError,
                lastCompileTime = snapshot?.GetProperty("lastCompileTime").GetString(),
                compilationDiagnostics = snapshot?.GetProperty("diagnostics"), diagnostics = diagnostics.Select(item => new
            {
                file = item.File,
                line = item.Line,
                character = item.Character,
                severity = item.IsError ? "error" : item.IsInfo ? "info" : "warning",
                location = $"{item.File}:{item.Line}:{item.Character}",
                message = item.Message,
            }).ToArray() };
        }
        else if (name == "as_project_info")
        {
            RequireNoArguments(arguments);
            if (!_debug.IsConnected)
                _ = await _debug.WaitForConnectionAsync(Math.Min(2000, _options.TimeoutMilliseconds)).WaitAsync(token).ConfigureAwait(false);
            _ = await _debug.WaitForDatabaseAsync(Math.Min(2000, _options.TimeoutMilliseconds)).WaitAsync(token).ConfigureAwait(false);
            await Task.Delay(250, token).ConfigureAwait(false);
            result = _projectInfo.Get(_debug);
        }
        else if (name == "as_debug_set_breakpoint")
        {
            RequireAllowed(arguments, "file", "line", "module");
            string file = RequiredString(arguments, "file");
            int line = RequiredInt(arguments, "line", 1, int.MaxValue);
            string module = OptionalString(arguments, "module");
            await _debug.EnsureDebuggingAsync(token).ConfigureAwait(false);
            await _debug.SetBreakpointAsync(file, line, module, token).ConfigureAwait(false);
            result = new { accepted = true, verified = false, file, line, module, note = "DebugServer has no acknowledgement for an unchanged breakpoint; rejection/remapping events are reported asynchronously." };
        }
        else if (name == "as_debug_clear_breakpoints")
        {
            RequireAllowed(arguments, "file", "module");
            string file = RequiredString(arguments, "file");
            string module = OptionalString(arguments, "module");
            await _debug.EnsureDebuggingAsync(token).ConfigureAwait(false);
            await _debug.ClearBreakpointsAsync(file, module, token).ConfigureAwait(false);
            result = new { accepted = true, file, module };
        }
        else if (name == "as_debug_continue")
        {
            RequireNoArguments(arguments);
            await _debug.EnsureDebuggingAsync(token).ConfigureAwait(false);
            await _debug.ContinueAsync(token).ConfigureAwait(false);
            result = new { accepted = true, action = "continue" };
        }
        else if (name == "as_debug_step")
        {
            RequireAllowed(arguments, "kind");
            string kind = RequiredString(arguments, "kind");
            if (kind is not ("in" or "over" or "out")) throw new ArgumentException("kind must be in, over, or out");
            await _debug.EnsureDebuggingAsync(token).ConfigureAwait(false);
            await _debug.StepAsync(kind, token).ConfigureAwait(false);
            result = new { action = "step_" + kind, stopped = true, reason = _debug.StopReason };
        }
        else if (name == "as_debug_stack")
        {
            RequireNoArguments(arguments);
            await _debug.EnsureDebuggingAsync(token).ConfigureAwait(false);
            IReadOnlyList<CallFrame> frames = await _debug.RequestStackAsync(token).ConfigureAwait(false);
            result = new { frames = frames.Select((frame, index) => new { id = index, frame.Name, frame.Source, frame.Line, frame.Module }).ToArray() };
        }
        else if (name == "as_debug_variables")
        {
            RequireAllowed(arguments, "path");
            string path = RequiredString(arguments, "path");
            await _debug.EnsureDebuggingAsync(token).ConfigureAwait(false);
            IReadOnlyList<DebugVariable> variables = await _debug.RequestVariablesAsync(path, token).ConfigureAwait(false);
            result = new { path, variables = variables.Select(FormatVariable).ToArray() };
        }
        else if (name == "as_debug_eval")
        {
            RequireAllowed(arguments, "expression", "frame");
            string expression = RequiredString(arguments, "expression");
            int frame = arguments.TryGetProperty("frame", out _) ? RequiredInt(arguments, "frame", 0, int.MaxValue) : 0;
            await _debug.EnsureDebuggingAsync(token).ConfigureAwait(false);
            DebugVariable? value = await _debug.EvaluateAsync(expression, frame, token).ConfigureAwait(false);
            result = new { expression, value = value is null ? null : FormatVariable(value) };
        }
        else throw new ArgumentException("unknown tool: " + name);

        return ToolResult(result, false);
    }

    private object MakeInitializeResult() => new
    {
        protocolVersion = "2024-11-05",
        capabilities = new { tools = new { listChanged = false } },
        serverInfo = new { name = "SoloASMcpHost", version = "0.2.0" },
    };

    private async Task<object> MakeToolsListAsync()
    {
        object[] serviceTools =
        [
            Tool("as_edit_begin", "Pause automatic script compilation while editing; the lease expires automatically.", Schema(new { timeoutSeconds = new { type = "integer", minimum = 1, maximum = 600 } }, [])),
            Tool("as_edit_end", "End this client's edit lease and wait for the resulting compilation.", Schema(new { leaseId = StringSchema() }, ["leaseId"])),
            Tool("as_compile", "Compile scripts and return this compilation's result and diagnostics.", new { type = "object", properties = new { }, additionalProperties = false }),
            Tool("as_diagnostics", "Read current AngelScript compiler errors and warnings pushed by the connected Editor DebugServer.", new { type = "object", properties = new { }, additionalProperties = false }),
            Tool("as_project_info", "Read project, engine, enabled plugins/modules, AngelScript roots/config, targets, maps, and live compile diagnostics.", new { type = "object", properties = new { }, additionalProperties = false }),
            Tool("as_debug_set_breakpoint", "Set an AngelScript source breakpoint in the running Editor.", Schema(new { file = StringSchema(), line = IntegerSchema(1), module = StringSchema() }, ["file", "line"])),
            Tool("as_debug_clear_breakpoints", "Clear AngelScript source breakpoints for a file in the running Editor.", Schema(new { file = StringSchema(), module = StringSchema() }, ["file"])),
            Tool("as_debug_continue", "Continue AngelScript execution in the running Editor.", new { type = "object", properties = new { }, additionalProperties = false }),
            Tool("as_debug_step", "Step into, over, or out of the current AngelScript frame.", Schema(new { kind = new { type = "string", @enum = new[] { "in", "over", "out" } }, }, ["kind"])),
            Tool("as_debug_stack", "Read the current AngelScript call stack; execution must be stopped.", new { type = "object", properties = new { }, additionalProperties = false }),
            Tool("as_debug_variables", "Read variables for a DebugServer expression path such as 0:%local% or 0:%this%.", Schema(new { path = StringSchema() }, ["path"])),
            Tool("as_debug_eval", "Evaluate an AngelScript debugger expression in a stack frame.", Schema(new { expression = StringSchema(), frame = IntegerSchema(0) }, ["expression"])),
        ];
        using CancellationTokenSource timeout = new(_options.TimeoutMilliseconds);
        JsonElement response = await _external.ProviderAsync(new { schemaVersion = 1, operation = "list" }, timeout.Token).ConfigureAwait(false);
        if (!response.GetProperty("ok").GetBoolean())
            throw new InvalidOperationException(response.GetProperty("error").GetRawText());
        List<object> tools = RuntimeApiCatalog.ToolDefinitions().Concat(serviceTools).Concat(ExternalTools.PackageToolDefinitions()).ToList();
        HashSet<string> names = new(RuntimeApiCatalog.ToolDefinitions().Concat(serviceTools).Concat(ExternalTools.PackageToolDefinitions())
            .Select(tool => JsonSerializer.SerializeToElement(tool).GetProperty("name").GetString()!), StringComparer.Ordinal);
        foreach (JsonElement provider in response.GetProperty("data").GetProperty("mcpProviders").EnumerateArray())
        {
            string providerId = provider.GetProperty("providerId").GetString()!;
            foreach (JsonElement tool in provider.GetProperty("tools").EnumerateArray())
            {
                string name = tool.GetProperty("name").GetString()!;
                if (!name.StartsWith(providerId + ".", StringComparison.Ordinal) || !names.Add(name))
                    throw new InvalidOperationException("duplicate_or_invalid_provider_tool");
                tools.Add(new { name, description = tool.GetProperty("description").GetString(),
                    inputSchema = tool.GetProperty("inputSchema").Clone(), annotations = new {
                        readOnlyHint = tool.GetProperty("readOnly").GetBoolean(),
                        destructiveHint = tool.GetProperty("destructive").GetBoolean(),
                    } });
            }
        }
        return new { tools = tools.ToArray() };
    }

    private static object Tool(string name, string description, object inputSchema) => new {
        name, description, inputSchema, annotations = new {
            readOnlyHint = name is "as_diagnostics" or "as_project_info" or "as_debug_stack" or "as_debug_variables",
            destructiveHint = name == "as_debug_eval",
        }
    };
    private static object FormatVariable(DebugVariable value) => new
    {
        name = value.Name,
        value = value.Value,
        type = value.Type,
        has_members = value.HasMembers,
    };
    private static object StringSchema() => new { type = "string" };
    private static object IntegerSchema(int minimum) => new { type = "integer", minimum };
    private static object Schema(object properties, string[] required) => new { type = "object", properties, required, additionalProperties = false };

    private static object ToolResult(object structuredContent, bool isError)
    {
        string text = JsonSerializer.Serialize(structuredContent);
        return new { content = new[] { new { type = "text", text } }, structuredContent, isError };
    }

    private static byte[] RpcResult(JsonElement id, object result)
    {
        byte[] json = JsonSerializer.SerializeToUtf8Bytes(new { jsonrpc = "2.0", id, result });
        return json.Append((byte)'\n').ToArray();
    }

    private static byte[] RpcError(JsonElement id, int code, string message)
    {
        byte[] json = JsonSerializer.SerializeToUtf8Bytes(new { jsonrpc = "2.0", id, error = new { code, message } });
        return json.Append((byte)'\n').ToArray();
    }

    private static void RequireObject(JsonElement arguments)
    {
        if (arguments.ValueKind != JsonValueKind.Object) throw new ArgumentException("arguments must be an object");
    }

    private static void RequireNoArguments(JsonElement arguments)
    {
        if (arguments.ValueKind != JsonValueKind.Object || arguments.EnumerateObject().Any()) throw new ArgumentException("tool accepts no arguments");
    }

    private static void RequireAllowed(JsonElement arguments, params string[] allowed)
    {
        if (arguments.ValueKind != JsonValueKind.Object) throw new ArgumentException("arguments must be an object");
        if (arguments.EnumerateObject().GroupBy(p => p.Name).Any(g => g.Count() > 1)) throw new ArgumentException("duplicate argument");
        foreach (JsonProperty property in arguments.EnumerateObject())
            if (!allowed.Contains(property.Name, StringComparer.Ordinal)) throw new ArgumentException("unknown argument: " + property.Name);
    }

    private static string RequiredString(JsonElement arguments, string name)
    {
        if (!arguments.TryGetProperty(name, out JsonElement value) || value.ValueKind != JsonValueKind.String || string.IsNullOrWhiteSpace(value.GetString()))
            throw new ArgumentException(name + " is required and must be a non-empty string");
        return value.GetString()!;
    }

    private static string OptionalString(JsonElement arguments, string name)
    {
        if (!arguments.TryGetProperty(name, out JsonElement value)) return string.Empty;
        if (value.ValueKind != JsonValueKind.String) throw new ArgumentException(name + " must be a string");
        return value.GetString() ?? string.Empty;
    }

    private static int RequiredInt(JsonElement arguments, string name, int minimum, int maximum)
    {
        if (!arguments.TryGetProperty(name, out JsonElement value) || value.ValueKind != JsonValueKind.Number || !value.TryGetInt32(out int number) || number < minimum || number > maximum)
            throw new ArgumentException($"{name} must be an integer from {minimum} to {maximum}");
        return number;
    }

    private static string ResolveEngineRoot(string value)
    {
        string full = Path.GetFullPath(value);
        if (Path.GetFileName(full).Equals("UnrealEditor-Cmd.exe", StringComparison.OrdinalIgnoreCase))
        {
            for (int i = 0; i < 4; ++i) full = Path.GetDirectoryName(full) ?? full;
        }
        return full;
    }

    public async ValueTask DisposeAsync()
    {
        await _edit.DisposeAsync().ConfigureAwait(false);
        await _debug.DisposeAsync().ConfigureAwait(false);
    }
}

internal sealed record HostOptions(string EnginePath, string ProjectPath, int TimeoutMilliseconds,
    string DebugHost, int DebugPort, string Lane, string PythonPath);
