using System.Diagnostics;
using System.Text;
using System.Text.Json;

namespace SoloASMcpHost;

// Both callers use the public CLI/file bridge; no packaging state is maintained here.
internal sealed class ExternalTools(HostOptions options)
{
    private readonly string _project = Path.GetFullPath(options.ProjectPath);

    private static readonly Dictionary<string, string[]> Commands = new(StringComparer.Ordinal) {
        ["profiles"] = ["profiles", "list"], ["profiles_save"] = ["profiles", "save"],
        ["config"] = ["config", "read"], ["config_write"] = ["config", "write"],
        ["precheck"] = ["precheck"], ["run"] = ["run"], ["status"] = ["status"],
        ["cancel"] = ["cancel"], ["logs"] = ["logs"], ["clean_cache"] = ["clean-cache"],
        ["generate_precompiled"] = ["generate-precompiled"], ["sync_jit"] = ["sync-jit"], ["check_package"] = ["check-package"],
    };

    public static IEnumerable<object> PackageToolDefinitions()
    {
        object Text() => new { type = "string", minLength = 1 };
        foreach (string action in Commands.Keys)
        {
            Dictionary<string, object> properties = new();
            string[] required = [];
            if (action is "status" or "cancel" or "logs")
            {
                properties["job"] = new { type = "string", pattern = "^[0-9a-f]{32}$" };
                required = ["job"];
                if (action == "logs") {
                    properties["offset"] = new { type = "integer", minimum = 0 };
                    properties["limit"] = new { type = "integer", minimum = 4, maximum = 1_048_576 };
                }
            }
            else if (action == "profiles_save")
            {
                properties["name"] = Text(); properties["description"] = new { type = "string" };
                properties["overrides"] = new { type = "object" }; properties["replace"] = new { type = "boolean" };
                required = ["name", "overrides"];
            }
            else if (action == "check_package")
            {
                properties["path"] = Text(); properties["mode"] = new { type = "string", @enum = new[] { "auto", "source", "bytecode", "semi_aot" } };
                required = ["path"];
            }
            else if (action != "profiles")
            {
                properties["platform"] = new { type = "string", @enum = new[] { "Win64", "Mac", "Linux" } };
                properties["overrides"] = new { type = "object" };
                if (action == "config_write") {
                    properties["dryRun"] = new { type = "boolean" }; required = ["overrides", "dryRun"];
                }
                else properties["profile"] = Text();
                if (action == "run") properties["jobId"] = new { type = "string", pattern = "^[0-9a-f]{32}$" };
                if (action == "precheck") properties["editor"] = Text();
            }
            yield return new {
                name = "soloas_package_" + action,
                description = action switch {
                    "profiles" => "List the project's saved packaging profiles using the SoloAS packaging CLI.",
                    "profiles_save" => "Save a named packaging profile; replace=true explicitly permits replacing an existing name. Does not edit project ini.",
                    "config" => "Read effective packaging configuration and its sources; does not save settings.",
                    "config_write" => "Preview a precise project ini diff with dryRun=true; explicitly set dryRun=false to save the selected settings.",
                    "precheck" => "Run packaging prechecks; optional editor probes the toolchain. Inspect readyToPackage and pendingChecks.",
                    "run" => "Run packaging synchronously. Optional jobId permits another session to poll/cancel. Set a suitable Host timeout.",
                    "status" => "Read a persisted packaging job, including failed or cancelled terminal state.",
                    "logs" => "Read bounded UTF-8 job log bytes; use returned nextOffset, not character count, for the next read.",
                    "cancel" => "Persist a cancellation request via the CLI. Success acknowledges delivery; only final job state confirms cleanup and termination.",
                    "clean_cache" => "Clean old caches using the shared packaging backend; may remove generated project files.",
                    "generate_precompiled" => "Generate precompiled data through the shared packaging backend; may compile and run owned processes.",
                    "sync_jit" => "Synchronize JIT through the shared packaging backend; may update generated files.",
                    _ => "Inspect an existing package through the shared backend; a successful query alone does not prove a package execution scenario.",
                },
                inputSchema = new { type = "object", properties, required, additionalProperties = false },
                annotations = new { readOnlyHint = action is "profiles" or "config" or "status" or "logs" or "check_package",
                    destructiveHint = action is "run" or "cancel" or "profiles_save" or "config_write" or "clean_cache" or "generate_precompiled" or "sync_jit" },
            };
        }
    }

    public async Task<JsonElement> PackageAsync(string name, JsonElement arguments, CancellationToken token)
    {
        string action = name["soloas_package_".Length..];
        if (!Commands.TryGetValue(action, out string[]? command)) throw new ArgumentException("unknown tool: " + name);
        if (arguments.ValueKind != JsonValueKind.Object) throw new ArgumentException("arguments must be an object");
        if (arguments.EnumerateObject().GroupBy(p => p.Name).Any(g => g.Count() > 1)) throw new ArgumentException("duplicate argument");
        JsonElement schema = JsonSerializer.SerializeToElement(PackageToolDefinitions().Single(t =>
            JsonSerializer.SerializeToElement(t).GetProperty("name").GetString() == name)).GetProperty("inputSchema");
        foreach (JsonElement required in schema.GetProperty("required").EnumerateArray())
            if (!arguments.TryGetProperty(required.GetString()!, out _)) throw new ArgumentException(required.GetString() + " is required");
        List<string> args = [PackagingCliPath(), "--project", _project, "--json"];
        args.AddRange(command);
        foreach (JsonProperty property in arguments.EnumerateObject())
        {
            if (!schema.GetProperty("properties").TryGetProperty(property.Name, out JsonElement rule)) throw new ArgumentException("unknown argument: " + property.Name);
            string type = rule.GetProperty("type").GetString()!;
            string value;
            if (type == "object" && property.Value.ValueKind == JsonValueKind.Object) value = property.Value.GetRawText();
            else if (type == "string" && property.Value.ValueKind == JsonValueKind.String
                && (!rule.TryGetProperty("minLength", out _) || !string.IsNullOrWhiteSpace(property.Value.GetString()))) value = property.Value.GetString()!;
            else if (type == "integer" && property.Value.ValueKind == JsonValueKind.Number && property.Value.TryGetInt32(out int number)) value = number.ToString(System.Globalization.CultureInfo.InvariantCulture);
            else if (type == "boolean" && property.Value.ValueKind is JsonValueKind.True or JsonValueKind.False) {
                if (!property.Value.GetBoolean()) continue;
                value = string.Empty;
            }
            else throw new ArgumentException(property.Name + " must be " + type);
            args.Add("--" + (property.Name switch { "jobId" => "job-id", "dryRun" => "dry-run", _ => property.Name }));
            if (type != "boolean") args.Add(value);
        }
        string directory = NewRequestDirectory();
        string response = Path.Combine(directory, "stdout.json");
        int exit = await RunAsync(options.PythonPath, args, directory, response, token).ConfigureAwait(false);
        return ReadEnvelope(response, exit);
    }

    public async Task<JsonElement> ProviderAsync(object request, CancellationToken token)
    {
        string directory = NewRequestDirectory();
        string input = Path.Combine(directory, "request.json");
        string output = Path.Combine(directory, "response.json");
        byte[] bytes = JsonSerializer.SerializeToUtf8Bytes(request);
        if (bytes.Length > 1_048_576) throw new ArgumentException("provider_request_overlimit");
        await File.WriteAllBytesAsync(input, bytes, token).ConfigureAwait(false);
        string executable = EditorExecutable(options.EnginePath);
        int exit = await RunAsync(executable, [ _project, "-run=SoloASProviders", "-RequestFile=" + input,
            "-ResponseFile=" + output, "-unattended", "-nocrashreports", "-NoMessaging", "-NoASDebugServer", "-NullRHI", "-NoSound", "-NoSplash",
            "-abslog=" + Path.Combine(directory, "editor.log") ], directory, Path.Combine(directory, "stdout.log"), token).ConfigureAwait(false);
        return ReadEnvelope(output, exit);
    }

    private string NewRequestDirectory()
    {
        string root = Path.Combine(Path.GetDirectoryName(_project)!, "Saved", "SoloASDeveloperTools", "Mcp", "requests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        return root;
    }

    private static string PackagingCliPath()
    {
        // Published Host: <plugin>/Binaries/ThirdParty/SoloASMcpHost/<platform>/.
        DirectoryInfo? root = new(AppContext.BaseDirectory);
        while (root is not null)
        {
            string path = Path.Combine(root.FullName, "Build", "PackagingTool", "soloas_package.py");
            if (File.Exists(path)) return path;
            root = root.Parent;
        }
        throw new FileNotFoundException("SoloAS packaging CLI was not found beside the installed plugin");
    }

    private static string EditorExecutable(string engine)
    {
        string full = Path.GetFullPath(engine);
        if (File.Exists(full)) return full;
        return OperatingSystem.IsWindows() ? Path.Combine(full, "Engine", "Binaries", "Win64", "UnrealEditor-Cmd.exe")
            : OperatingSystem.IsMacOS() ? Path.Combine(full, "Engine", "Binaries", "Mac", "UnrealEditor-Cmd")
            : Path.Combine(full, "Engine", "Binaries", "Linux", "UnrealEditor-Cmd");
    }

    private static async Task<int> RunAsync(string executable, IEnumerable<string> arguments, string directory, string stdout, CancellationToken token)
    {
        ProcessStartInfo start = new(executable) { UseShellExecute = false, CreateNoWindow = true,
            WorkingDirectory = directory, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (string argument in arguments) start.ArgumentList.Add(argument);
        using Process process = new() { StartInfo = start };
        await using FileStream output = File.Create(stdout);
        await using FileStream error = File.Create(Path.Combine(directory, "stderr.log"));
        if (!process.Start()) throw new IOException("child_process_did_not_start");
        Task outTask = process.StandardOutput.BaseStream.CopyToAsync(output);
        Task errTask = process.StandardError.BaseStream.CopyToAsync(error);
        try { await process.WaitForExitAsync(token).ConfigureAwait(false); }
        catch (OperationCanceledException)
        {
            if (!process.HasExited) process.Kill(entireProcessTree: true);
            await process.WaitForExitAsync().ConfigureAwait(false);
            await Task.WhenAll(outTask, errTask).ConfigureAwait(false);
            throw;
        }
        await Task.WhenAll(outTask, errTask).ConfigureAwait(false);
        return process.ExitCode;
    }

    private static JsonElement ReadEnvelope(string path, int exit)
    {
        FileInfo file = new(path);
        if (!file.Exists || file.Length is < 2 or > 16_777_216) throw new IOException("backend_response_missing_or_overlimit: exit=" + exit);
        using JsonDocument document = JsonDocument.Parse(File.ReadAllBytes(path));
        JsonElement root = document.RootElement;
        if (root.ValueKind != JsonValueKind.Object
            || !root.TryGetProperty("schemaVersion", out JsonElement version) || version.ValueKind != JsonValueKind.Number || !version.TryGetInt32(out int number) || number != 1
            || !root.TryGetProperty("ok", out JsonElement ok) || ok.ValueKind is not (JsonValueKind.True or JsonValueKind.False)
            || !root.TryGetProperty("data", out _) || !root.TryGetProperty("error", out _)
            || (exit == 0) != ok.GetBoolean())
            throw new IOException("backend_response_exit_mismatch: exit=" + exit);
        return root.Clone();
    }
}
