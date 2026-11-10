// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace SoloASMcpHost;

internal sealed class EditClient : IAsyncDisposable
{
    private enum SessionAttempt { None, Malformed, Rejected }
    private readonly string _sessionDirectory;
    private readonly string _sessionPattern;
    private readonly bool _localDebug;
    private readonly DebugServerClient _debug;
    private readonly CancellationTokenSource _shutdown = new();
    private readonly SemaphoreSlim _sendLock = new(1, 1);
    private readonly ConcurrentDictionary<string, TaskCompletionSource<JsonElement>> _requests = new();
    private readonly object _gate = new();
    private readonly Task _loop;
    private TcpClient? _client;
    private NetworkStream? _stream;
    private bool _ready;
    private JsonElement? _snapshot;
    private string? _lastError;

    public EditClient(string projectRoot, string debugHost, int debugPort, DebugServerClient debug)
    {
        _sessionDirectory = Path.Combine(projectRoot, "Saved", "SoloASDeveloperTools", "Mcp");
        _sessionPattern = $"edit-session-{debugPort}-*.json";
        _localDebug = IsSupportedDebugHost(debugHost);
        _debug = debug;
        _debug.ConnectionLost += Disconnect;
        _loop = ConnectLoopAsync(_shutdown.Token);
    }

    public JsonElement? Snapshot { get { lock (_gate) return _snapshot?.Clone(); } }
    public string? LastError { get { lock (_gate) return _lastError; } }

    public async Task<JsonElement> CallAsync(string operation, object? arguments, CancellationToken cancellationToken)
    {
        if (!_localDebug) throw new InvalidOperationException("batch editing requires a local IPv4 DebugServer connection");
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            lock (_gate)
            {
                if (_ready) break;
                if (_lastError is not null) throw new InvalidOperationException(_lastError);
            }
            await Task.Delay(50, cancellationToken).ConfigureAwait(false);
        }
        JsonObject message = arguments is null ? new JsonObject() : JsonSerializer.SerializeToNode(arguments)!.AsObject();
        string id = Guid.NewGuid().ToString("N");
        message["id"] = id;
        message["op"] = operation;
        TaskCompletionSource<JsonElement> completion = new(TaskCreationOptions.RunContinuationsAsynchronously);
        if (!_requests.TryAdd(id, completion)) throw new InvalidOperationException("duplicate request");
        try
        {
            await SendAsync(message, cancellationToken).ConfigureAwait(false);
            JsonElement response = await completion.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
            if (response.TryGetProperty("error", out JsonElement error)) throw new InvalidOperationException(error.GetString());
            return response;
        }
        catch
        {
            // A lost begin response must never leave a lease owned by this host.
            Disconnect();
            throw;
        }
        finally { _requests.TryRemove(id, out _); }
    }

    private async Task SendAsync(JsonObject message, CancellationToken token)
    {
        NetworkStream stream;
        lock (_gate) stream = _stream ?? throw new InvalidOperationException("edit channel disconnected");
        byte[] bytes = Encoding.UTF8.GetBytes(message.ToJsonString() + "\n");
        await _sendLock.WaitAsync(token).ConfigureAwait(false);
        try { await stream.WriteAsync(bytes, token).ConfigureAwait(false); }
        finally { _sendLock.Release(); }
    }

    private async Task ConnectLoopAsync(CancellationToken token)
    {
        string? previousEndpoint = null;
        int rejectedRounds = 0;
        while (!token.IsCancellationRequested)
        {
            try
            {
                string? endpoint = _localDebug ? _debug.LocalEndpoint : null;
                if (endpoint != previousEndpoint) { previousEndpoint = endpoint; rejectedRounds = 0; }
                if (endpoint is not null)
                {
                    bool malformed = false;
                    bool rejected = false;
                    foreach (string session in Directory.EnumerateFiles(_sessionDirectory, _sessionPattern).OrderBy(p => p, StringComparer.Ordinal))
                    {
                        if (_debug.LocalEndpoint != endpoint) break;
                        SessionAttempt result = await TrySessionAsync(session, endpoint, token).ConfigureAwait(false);
                        malformed |= result == SessionAttempt.Malformed;
                        rejected |= result == SessionAttempt.Rejected;
                    }
                    if (malformed && _debug.LocalEndpoint == endpoint) ReportMalformedResponse();
                    if (rejected && _debug.LocalEndpoint == endpoint)
                    {
                        if (++rejectedRounds >= 3 && !malformed) ReportHandshakeRejected();
                    }
                    else rejectedRounds = 0;
                }
            }
            catch (OperationCanceledException) when (token.IsCancellationRequested) { }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ObjectDisposedException) { }
            if (!token.IsCancellationRequested)
                try { await Task.Delay(200, token).ConfigureAwait(false); } catch (OperationCanceledException) { }
        }
    }

    private async Task<SessionAttempt> TrySessionAsync(string path, string debugEndpoint, CancellationToken token)
    {
        using TcpClient client = new();
        bool published = false;
        bool malformed = false;
        bool rejected = false;
        bool helloSent = false;
        try
        {
            using JsonDocument session = JsonDocument.Parse(await File.ReadAllTextAsync(path, token).ConfigureAwait(false));
            int port = session.RootElement.GetProperty("port").GetInt32();
            string secret = session.RootElement.GetProperty("token").GetString()!;
            using CancellationTokenSource handshake = CancellationTokenSource.CreateLinkedTokenSource(token);
            handshake.CancelAfter(TimeSpan.FromSeconds(2));
            await client.ConnectAsync(IPAddress.Loopback, port, handshake.Token).ConfigureAwait(false);
            NetworkStream stream = client.GetStream();
            JsonObject hello = MakeHello(secret, debugEndpoint);
            await stream.WriteAsync(Encoding.UTF8.GetBytes(hello.ToJsonString() + "\n"), handshake.Token).ConfigureAwait(false);
            helloSent = true;
            using StreamReader reader = new(stream, new UTF8Encoding(false, true), false, 4096, true);
            string? first = await reader.ReadLineAsync(handshake.Token).ConfigureAwait(false);
            if (first is null) return SessionAttempt.Rejected;
            using JsonDocument response = JsonDocument.Parse(first);
            JsonElement snapshot = response.RootElement.Clone();
            ValidateSnapshot(snapshot);
            if (!snapshot.TryGetProperty("id", out JsonElement id) || id.ValueKind != JsonValueKind.String || id.GetString() != "hello")
                throw new JsonException("invalid edit handshake");
            if (_debug.LocalEndpoint != debugEndpoint) return SessionAttempt.None;
            lock (_gate)
            {
                _client = client;
                _stream = stream;
                _snapshot = snapshot;
                _ready = true;
                _lastError = null;
                published = true;
            }
            while (_debug.LocalEndpoint == debugEndpoint && await reader.ReadLineAsync(token).ConfigureAwait(false) is { } line)
            {
                using JsonDocument document = JsonDocument.Parse(line);
                JsonElement value = document.RootElement.Clone();
                ValidateSnapshot(value);
                lock (_gate) _snapshot = value;
                if (value.TryGetProperty("id", out JsonElement requestId) && requestId.ValueKind == JsonValueKind.String
                    && _requests.TryGetValue(requestId.GetString()!, out TaskCompletionSource<JsonElement>? completion))
                    completion.TrySetResult(value);
            }
        }
        catch (OperationCanceledException) when (!token.IsCancellationRequested) { }
        catch (JsonException)
        {
            malformed = true;
            if (published) ReportMalformedResponse();
        }
        catch (Exception ex) when (ex is IOException or SocketException or KeyNotFoundException or InvalidOperationException or UnauthorizedAccessException or ObjectDisposedException or ArgumentException)
        {
            rejected = helloSent && !published && (ex is IOException or SocketException);
        }
        finally
        {
            if (published)
            {
                lock (_gate)
                {
                    if (ReferenceEquals(_client, client)) { _client = null; _stream = null; _snapshot = null; _ready = false; }
                }
                foreach (TaskCompletionSource<JsonElement> request in _requests.Values)
                    request.TrySetException(new IOException(malformed ? "edit_channel_invalid_json" : "edit channel disconnected"));
            }
        }
        return malformed ? SessionAttempt.Malformed : rejected ? SessionAttempt.Rejected : SessionAttempt.None;
    }

    internal static JsonObject MakeHello(string secret, string debugEndpoint) =>
        new() { ["id"] = "hello", ["op"] = "hello", ["token"] = secret, ["debugEndpoint"] = debugEndpoint };

    internal static bool IsSupportedDebugHost(string host)
    {
        if (string.Equals(host, "localhost", StringComparison.OrdinalIgnoreCase)) return true;
        if (!IPAddress.TryParse(host, out IPAddress? address)) return false;
        if (address.IsIPv4MappedToIPv6) address = address.MapToIPv4();
        return address.AddressFamily == AddressFamily.InterNetwork && IPAddress.IsLoopback(address);
    }

    internal static void ValidateSnapshot(JsonElement value)
    {
        if (value.ValueKind != JsonValueKind.Object
            || !value.TryGetProperty("generation", out JsonElement generation) || generation.ValueKind != JsonValueKind.Number || !generation.TryGetInt64(out long number) || number < 0
            || !value.TryGetProperty("state", out JsonElement state) || state.ValueKind != JsonValueKind.String
            || state.GetString() is not ("idle" or "pending" or "compiling" or "done")
            || !value.TryGetProperty("lastCompileTime", out JsonElement time) || time.ValueKind != JsonValueKind.String
            || !value.TryGetProperty("success", out JsonElement success) || success.ValueKind is not (JsonValueKind.True or JsonValueKind.False)
            || !value.TryGetProperty("diagnostics", out JsonElement diagnostics) || diagnostics.ValueKind != JsonValueKind.Array
            || (value.TryGetProperty("error", out JsonElement error) && error.ValueKind != JsonValueKind.String))
            throw new JsonException("invalid edit response");
    }

    internal void ReportMalformedResponse()
    {
        ReportChannelError("edit_channel_invalid_json", "Edit channel protocol error: invalid JSON response or session metadata.");
    }

    internal void ReportHandshakeRejected()
    {
        ReportChannelError("edit_channel_handshake_rejected", "Edit channel handshake was rejected by all reachable candidates.");
    }

    private void ReportChannelError(string code, string message)
    {
        lock (_gate)
        {
            if (_lastError == code) return;
            _lastError = code;
        }
        Console.Error.WriteLine(message);
    }

    private void Disconnect()
    {
        lock (_gate) { _client?.Dispose(); _client = null; _stream = null; _snapshot = null; _ready = false; _lastError = null; }
    }

    public async ValueTask DisposeAsync()
    {
        _debug.ConnectionLost -= Disconnect;
        _shutdown.Cancel();
        Disconnect();
        await _loop.ConfigureAwait(false);
        _shutdown.Dispose();
        _sendLock.Dispose();
    }
}
