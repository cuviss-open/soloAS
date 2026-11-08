using System.Text;
using System.Text.Json;

namespace SoloASMcpHost;

internal static class Program
{
    private const int DefaultTimeoutMilliseconds = 120_000;
    private const int MinimumTimeoutMilliseconds = 1_000;
    private const int MaximumTimeoutMilliseconds = 5_400_000;
    private const int MaximumRequestBytes = 1_048_576;
    private const long MaximumExactJsonRpcInteger = 9_007_199_254_991;
    private static readonly UTF8Encoding Utf8WithoutBom = new(false, true);

    private static async Task<int> Main(string[] args)
    {
        if (!TryParseOptions(args, out HostOptions? options, out string? optionError))
        {
            await Console.Error.WriteLineAsync(optionError);
            return 2;
        }

        Console.OutputEncoding = Utf8WithoutBom;
        using Stream input = Console.OpenStandardInput();
        using Stream output = Console.OpenStandardOutput();

        try
        {
            await using McpHostService service = new(options!);
            await foreach (RequestRecord record in ReadRequestRecordsAsync(input))
            {
                if (record.FailureReason is not null)
                {
                    await WriteTransportErrorAsync(output, RequestClassification.Invalid.CorrelationId, record.FailureReason);
                    continue;
                }

                RequestClassification classification = ClassifyRequest(record.Bytes);
                if (classification.HasUnsupportedId)
                {
                    await WriteTransportErrorAsync(output, RequestClassification.Invalid.CorrelationId, "request_id_not_supported");
                    continue;
                }

                byte[] servicePayload;
                try
                {
                    servicePayload = await service.HandleAsync(record.Bytes);
                }
                catch (Exception exception) when (exception is JsonException or InvalidOperationException)
                {
                    await WriteTransportErrorAsync(output, classification.CorrelationId, "request_invalid");
                    continue;
                }

                if (classification.IsNotification)
                {
                    if (servicePayload.Length != 0) await Console.Error.WriteLineAsync("notification response discarded");
                    continue;
                }

                if (!TryValidateHostResponse(servicePayload, classification.CorrelationId, out byte[] payload, out string reason))
                {
                    await WriteTransportErrorAsync(output, classification.CorrelationId, reason);
                    continue;
                }

                await output.WriteAsync(payload);
                await output.WriteAsync(new byte[] { (byte)'\n' });
                await output.FlushAsync();
            }
        }
        catch (IOException exception)
        {
            await Console.Error.WriteLineAsync($"host I/O failure: {exception.GetType().Name}");
            return 1;
        }

        return 0;
    }

    private static bool TryParseOptions(string[] args, out HostOptions? options, out string? error)
    {
        string? engine = null;
        string? project = null;
        string? python = null;
        string debugHost = "127.0.0.1";
        int debugPort = 27099;
        string lane = string.Empty;
        int timeoutMilliseconds = DefaultTimeoutMilliseconds;
        bool timeoutSpecified = false;
        bool debugHostSpecified = false;
        bool debugPortSpecified = false;

        for (int index = 0; index < args.Length; index += 2)
        {
            if (index + 1 >= args.Length)
            {
                options = null;
                error = "each host option requires a value";
                return false;
            }

            string name = args[index];
            string value = args[index + 1];
            if (string.IsNullOrWhiteSpace(value))
            {
                options = null;
                error = $"{name} must not be empty";
                return false;
            }

            switch (name)
            {
                case "--engine" when engine is null:
                    engine = value;
                    break;
                case "--project" when project is null:
                    project = value;
                    break;
                case "--python" when python is null:
                    python = value;
                    break;
                case "--debug-host" when !debugHostSpecified:
                    debugHost = value;
                    debugHostSpecified = true;
                    break;
                case "--debug-port" when !debugPortSpecified:
                    if (!int.TryParse(value, out debugPort) || debugPort is < 1 or > 65535)
                    {
                        options = null;
                        error = "--debug-port must be between 1 and 65535";
                        return false;
                    }
                    debugPortSpecified = true;
                    break;
                case "--lane" when lane.Length == 0:
                    if (value is not ("stock" or "haze"))
                    {
                        options = null;
                        error = "--lane must be stock or haze";
                        return false;
                    }
                    lane = value;
                    break;
                case "--timeout-ms" when !timeoutSpecified:
                    if (!int.TryParse(value, out timeoutMilliseconds)
                        || timeoutMilliseconds < MinimumTimeoutMilliseconds
                        || timeoutMilliseconds > MaximumTimeoutMilliseconds)
                    {
                        options = null;
                        error = $"--timeout-ms must be between {MinimumTimeoutMilliseconds} and {MaximumTimeoutMilliseconds}";
                        return false;
                    }

                    timeoutSpecified = true;
                    break;
                default:
                    options = null;
                    error = $"unknown or duplicate option: {name}";
                    return false;
            }
        }

        if (engine is null || project is null)
        {
            options = null;
            error = "--engine and --project are required";
            return false;
        }

        options = new HostOptions(engine, project, timeoutMilliseconds, debugHost, debugPort, lane, python ?? (OperatingSystem.IsWindows() ? "python" : "python3"));
        error = null;
        return true;
    }

    private static async IAsyncEnumerable<RequestRecord> ReadRequestRecordsAsync(Stream input)
    {
        byte[] buffer = new byte[16_384];
        using MemoryStream record = new();
        bool exceededLimit = false;

        int read;
        while ((read = await input.ReadAsync(buffer)) != 0)
        {
            for (int index = 0; index < read; ++index)
            {
                byte value = buffer[index];
                if (value == (byte)'\n')
                {
                    if (exceededLimit)
                    {
                        yield return RequestRecord.Failure("request_overlimit");
                    }
                    else
                    {
                        byte[] bytes = record.ToArray();
                        if (bytes.Length > 0 && bytes[^1] == (byte)'\r')
                        {
                            Array.Resize(ref bytes, bytes.Length - 1);
                        }
                        yield return RequestRecord.Success(bytes);
                    }

                    record.SetLength(0);
                    exceededLimit = false;
                    continue;
                }

                if (!exceededLimit)
                {
                    if (record.Length == MaximumRequestBytes)
                    {
                        exceededLimit = true;
                    }
                    else
                    {
                        record.WriteByte(value);
                    }
                }
            }
        }

        if (exceededLimit)
        {
            yield return RequestRecord.Failure("request_overlimit");
        }
        else if (record.Length != 0)
        {
            yield return RequestRecord.Failure("request_unterminated");
        }
    }

    private static RequestClassification ClassifyRequest(byte[] request)
    {
        try
        {
            if (request.AsSpan().StartsWith(new byte[] { 0xef, 0xbb, 0xbf }))
            {
                return RequestClassification.Invalid;
            }

            using JsonDocument document = JsonDocument.Parse(request);
            JsonElement root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
            {
                return RequestClassification.Invalid;
            }

            if (!root.TryGetProperty("id", out JsonElement id))
            {
                return IsValidNotificationEnvelope(root)
                    ? RequestClassification.Notification
                    : RequestClassification.Invalid;
            }

            if (!IsJsonRpcId(id))
            {
                return RequestClassification.Invalid;
            }

            if (id.ValueKind == JsonValueKind.Number && !IsExactJsonRpcInteger(id))
            {
                return RequestClassification.UnsupportedId;
            }

            return new RequestClassification(false, false, id.Clone());
        }
        catch (JsonException)
        {
            return RequestClassification.Invalid;
        }
    }

    private static bool IsValidNotificationEnvelope(JsonElement root)
    {
        if (!root.TryGetProperty("jsonrpc", out JsonElement version)
            || version.ValueKind != JsonValueKind.String
            || version.GetString() != "2.0"
            || !root.TryGetProperty("method", out JsonElement method)
            || method.ValueKind != JsonValueKind.String
            || string.IsNullOrEmpty(method.GetString()))
        {
            return false;
        }

        if (root.EnumerateObject().GroupBy(p => p.Name).Any(g => g.Count() > 1)) return false;

        return !root.TryGetProperty("params", out JsonElement parameters)
            || parameters.ValueKind == JsonValueKind.Object;
    }

    private static bool IsExactJsonRpcInteger(JsonElement id) =>
        id.TryGetInt64(out long value)
        && value >= -MaximumExactJsonRpcInteger
        && value <= MaximumExactJsonRpcInteger;

    private static bool TryValidateHostResponse(
        byte[] bytes,
        JsonElement expectedId,
        out byte[] payload,
        out string reason)
    {
        payload = Array.Empty<byte>();
        reason = "host_response_invalid";
        if (bytes.Length == 0)
        {
            reason = "host_response_empty";
            return false;
        }

        int payloadLength;
        if (bytes[^1] == (byte)'\n')
        {
            payloadLength = bytes.Length - 1;
            if (payloadLength > 0 && bytes[payloadLength - 1] == (byte)'\r')
            {
                --payloadLength;
            }
        }
        else
        {
            return false;
        }

        for (int index = 0; index < payloadLength; ++index)
        {
            if (bytes[index] == (byte)'\r' || bytes[index] == (byte)'\n')
            {
                reason = "host_response_multiline";
                return false;
            }
        }

        if (payloadLength == 0 || bytes[0] != (byte)'{' || bytes[payloadLength - 1] != (byte)'}')
        {
            return false;
        }

        byte[] candidate = bytes.AsSpan(0, payloadLength).ToArray();
        try
        {
            using JsonDocument document = JsonDocument.Parse(candidate);
            JsonElement root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object
                || CountProperties(root, "jsonrpc") != 1
                || CountProperties(root, "id") != 1
                || CountProperties(root, "result") + CountProperties(root, "error") != 1
                || !root.TryGetProperty("jsonrpc", out JsonElement version)
                || version.ValueKind != JsonValueKind.String
                || version.GetString() != "2.0"
                || !root.TryGetProperty("id", out JsonElement responseId)
                || !IsJsonRpcId(responseId)
                || (root.TryGetProperty("error", out JsonElement error) && !IsJsonRpcError(error)))
            {
                reason = "host_response_invalid";
                return false;
            }

            if (!IdsMatch(expectedId, responseId))
            {
                reason = "host_id_mismatch";
                return false;
            }

            payload = candidate;
            reason = string.Empty;
            return true;
        }
        catch (JsonException)
        {
            return false;
        }
    }

    private static int CountProperties(JsonElement objectElement, string name)
    {
        int count = 0;
        foreach (JsonProperty property in objectElement.EnumerateObject())
        {
            if (property.NameEquals(name))
            {
                ++count;
            }
        }

        return count;
    }

    private static bool IsJsonRpcId(JsonElement id) =>
        id.ValueKind is JsonValueKind.String or JsonValueKind.Number or JsonValueKind.Null;

    private static bool IsJsonRpcError(JsonElement error) =>
        error.ValueKind == JsonValueKind.Object
        && CountProperties(error, "code") == 1
        && CountProperties(error, "message") == 1
        && error.TryGetProperty("code", out JsonElement code)
        && code.ValueKind == JsonValueKind.Number
        && error.TryGetProperty("message", out JsonElement message)
        && message.ValueKind == JsonValueKind.String;

    private static bool IdsMatch(JsonElement expected, JsonElement actual)
    {
        try
        {
            if (expected.ValueKind != actual.ValueKind)
            {
                return false;
            }

            return expected.ValueKind switch
            {
                JsonValueKind.Null => true,
                JsonValueKind.String => string.Equals(expected.GetString(), actual.GetString(), StringComparison.Ordinal),
                JsonValueKind.Number => IsExactJsonRpcInteger(expected)
                    && IsExactJsonRpcInteger(actual)
                    && expected.GetInt64() == actual.GetInt64(),
                _ => false,
            };
        }
        catch (FormatException)
        {
            return false;
        }
    }

    private static async Task WriteTransportErrorAsync(Stream output, JsonElement id, string reason)
    {
        using MemoryStream serialized = new();
        using (Utf8JsonWriter writer = new(serialized))
        {
            writer.WriteStartObject();
            writer.WriteString("jsonrpc", "2.0");
            writer.WritePropertyName("id");
            JsonSerializer.Serialize(writer, id);
            writer.WritePropertyName("error");
            writer.WriteStartObject();
            writer.WriteNumber("code", -32000);
            writer.WriteString("message", "Host transport error");
            writer.WritePropertyName("data");
            writer.WriteStartObject();
            writer.WriteString("kind", "transport_error");
            writer.WriteString("reason", reason);
            writer.WriteEndObject();
            writer.WriteEndObject();
            writer.WriteEndObject();
            await writer.FlushAsync();
        }

        await output.WriteAsync(serialized.ToArray());
        await output.WriteAsync(new byte[] { (byte)'\n' });
        await output.FlushAsync();
    }

    private readonly record struct RequestClassification(bool IsNotification, bool HasUnsupportedId, JsonElement CorrelationId)
    {
        public static RequestClassification Invalid => new(false, false, JsonDocument.Parse("null").RootElement.Clone());
        public static RequestClassification Notification => new(true, false, JsonDocument.Parse("null").RootElement.Clone());
        public static RequestClassification UnsupportedId => new(false, true, JsonDocument.Parse("null").RootElement.Clone());
    }

    private readonly record struct RequestRecord(byte[] Bytes, string? FailureReason)
    {
        public static RequestRecord Success(byte[] bytes) => new(bytes, null);
        public static RequestRecord Failure(string reason) => new(Array.Empty<byte>(), reason);
    }

}
