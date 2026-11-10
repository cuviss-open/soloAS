// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace SoloASMcpHost;

internal sealed class DebugServerClient : IAsyncDisposable
{
    public event Action? ConnectionLost;
    private const int MaximumPacketBytes = 67_108_864;
    private const int MaximumCollectionEntries = 100_000;
    private readonly string _host;
    private readonly int _port;
    private readonly string _snapshotPath;
    private readonly string? _legacySnapshotPath;
    private readonly CancellationTokenSource _shutdown = new();
    private readonly SemaphoreSlim _sendLock = new(1, 1);
    private readonly object _stateLock = new();
    private readonly ConcurrentDictionary<byte, ConcurrentQueue<TaskCompletionSource<DebugMessage>>> _waiters = new();
    private readonly Task _connectionTask;
    private TcpClient? _client;
    private NetworkStream? _stream;
    private JsonObject _incomingDatabase = new();
    private JsonObject? _database;
    private JsonObject _settings = new();
    private List<DiagnosticRecord> _diagnostics = new();
    private List<CallFrame> _stack = new();
    private List<DebugVariable> _variables = new();
    private DebugVariable? _evaluation;
    private bool _databaseReady;
    private bool _startedDebugging;
    private int _serverVersion;
    private string? _lastStopReason;
    private string? _lastStopDescription;
    private string? _lastStopText;

    public DebugServerClient(string host, int port, string snapshotPath, string? legacySnapshotPath = null)
    {
        _host = host;
        _port = port;
        _snapshotPath = snapshotPath;
        _legacySnapshotPath = legacySnapshotPath;
        LoadSnapshot();
        _connectionTask = ConnectLoopAsync(_shutdown.Token);
    }

    public bool IsConnected { get { lock (_stateLock) return _stream is not null; } }
    public string? LocalEndpoint { get { lock (_stateLock) return NormalizeEndpoint(_client?.Client.LocalEndPoint); } }
    internal static string? NormalizeEndpoint(EndPoint? endpoint)
    {
        if (endpoint is not IPEndPoint ip) return null;
        IPAddress address = ip.Address.IsIPv4MappedToIPv6 ? ip.Address.MapToIPv4() : ip.Address;
        if (address.AddressFamily != AddressFamily.InterNetwork) return null;
        return new IPEndPoint(address, ip.Port).ToString();
    }
    public bool IsDatabaseReady { get { lock (_stateLock) return _databaseReady && _database is not null; } }
    public int ServerVersion { get { lock (_stateLock) return _serverVersion; } }
    public string? StopReason { get { lock (_stateLock) return _lastStopReason; } }
    public bool IsStopped { get { lock (_stateLock) return _lastStopReason is not null; } }
    public IReadOnlyList<DiagnosticRecord> Diagnostics { get { lock (_stateLock) return _diagnostics.ToArray(); } }
    public IReadOnlyList<CallFrame> Stack { get { lock (_stateLock) return _stack.ToArray(); } }
    public IReadOnlyList<DebugVariable> Variables { get { lock (_stateLock) return _variables.ToArray(); } }
    public DebugVariable? Evaluation { get { lock (_stateLock) return _evaluation; } }
    public JsonObject Settings { get { lock (_stateLock) return (JsonObject)_settings.DeepClone(); } }
    public JsonObject? Database { get { lock (_stateLock) return _database is null ? null : (JsonObject)_database.DeepClone(); } }

    public async Task<JsonObject?> WaitForDatabaseAsync(int timeoutMilliseconds)
    {
        if (!IsConnected) return Database;
        DateTime deadline = DateTime.UtcNow.AddMilliseconds(timeoutMilliseconds);
        while (DateTime.UtcNow < deadline)
        {
            if (IsDatabaseReady) return Database;
            if (!IsConnected) return Database;
            await Task.Delay(100, _shutdown.Token).ConfigureAwait(false);
        }
        JsonObject? final = Database;
        return !IsConnected || IsDatabaseReady ? final : null;
    }

    public async Task<bool> WaitForConnectionAsync(int timeoutMilliseconds)
    {
        DateTime deadline = DateTime.UtcNow.AddMilliseconds(timeoutMilliseconds);
        while (!IsConnected && DateTime.UtcNow < deadline)
            await Task.Delay(100, _shutdown.Token).ConfigureAwait(false);
        return IsConnected;
    }

    public async Task EnsureDebuggingAsync(CancellationToken cancellationToken)
    {
        if (!IsConnected) throw new InvalidOperationException("DebugServer is not connected");
        lock (_stateLock)
        {
            if (_startedDebugging) return;
            _startedDebugging = true;
        }
        Task<DebugMessage> version = WaitForMessageAsync(33, 3000, cancellationToken);
        await SendAsync(3, WriteInt32(2), cancellationToken).ConfigureAwait(false);
        try { _ = await version.ConfigureAwait(false); }
        catch (TimeoutException) { }
    }

    public async Task SendAsync(byte messageType, byte[] payload, CancellationToken cancellationToken)
    {
        NetworkStream stream;
        lock (_stateLock) stream = _stream ?? throw new InvalidOperationException("DebugServer is not connected");
        byte[] packet = new byte[5 + payload.Length];
        BinaryPrimitives.WriteInt32LittleEndian(packet.AsSpan(0, 4), 1 + payload.Length);
        packet[4] = messageType;
        payload.CopyTo(packet.AsSpan(5));
        await _sendLock.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            await stream.WriteAsync(packet, cancellationToken).ConfigureAwait(false);
            await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
        }
        finally { _sendLock.Release(); }
    }

    public async Task<DebugMessage> WaitForMessageAsync(byte messageType, int timeoutMilliseconds, CancellationToken cancellationToken)
    {
        TaskCompletionSource<DebugMessage> waiter = new(TaskCreationOptions.RunContinuationsAsynchronously);
        _waiters.GetOrAdd(messageType, _ => new ConcurrentQueue<TaskCompletionSource<DebugMessage>>()).Enqueue(waiter);
        using CancellationTokenSource timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(timeoutMilliseconds);
        try { return await waiter.Task.WaitAsync(timeout.Token).ConfigureAwait(false); }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            throw new TimeoutException($"DebugServer message {messageType} was not received");
        }
    }

    public async Task SetBreakpointAsync(string file, int line, string module, CancellationToken cancellationToken)
    {
        int id = Random.Shared.Next(1, int.MaxValue);
        using MemoryStream payload = new();
        WriteString(payload, file);
        WriteInt32(payload, line);
        WriteInt32(payload, id);
        WriteString(payload, module);
        await SendAsync(10, payload.ToArray(), cancellationToken).ConfigureAwait(false);
    }

    public Task ClearBreakpointsAsync(string file, string module, CancellationToken cancellationToken)
    {
        using MemoryStream payload = new();
        WriteString(payload, file);
        WriteString(payload, module);
        return SendAsync(9, payload.ToArray(), cancellationToken);
    }

    public Task ContinueAsync(CancellationToken cancellationToken) => SendAsync(6, Array.Empty<byte>(), cancellationToken);

    public async Task StepAsync(string kind, CancellationToken cancellationToken)
    {
        if (!IsStopped) throw new InvalidOperationException("AngelScript execution is not stopped");
        byte messageType = kind switch
        {
            "in" => 14,
            "over" => 13,
            "out" => 15,
            _ => throw new ArgumentException("kind must be in, over, or out", nameof(kind)),
        };
        Task<DebugMessage> stopped = WaitForMessageAsync(11, 120_000, cancellationToken);
        await SendAsync(messageType, Array.Empty<byte>(), cancellationToken).ConfigureAwait(false);
        _ = await stopped.ConfigureAwait(false);
    }

    public async Task<IReadOnlyList<CallFrame>> RequestStackAsync(CancellationToken cancellationToken)
    {
        if (!IsStopped) throw new InvalidOperationException("AngelScript execution is not stopped");
        Task<DebugMessage> response = WaitForMessageAsync(8, 3000, cancellationToken);
        await SendAsync(7, Array.Empty<byte>(), cancellationToken).ConfigureAwait(false);
        _ = await response.ConfigureAwait(false);
        lock (_stateLock) return _stack.ToArray();
    }

    public async Task<IReadOnlyList<DebugVariable>> RequestVariablesAsync(string path, CancellationToken cancellationToken)
    {
        if (!IsStopped) throw new InvalidOperationException("AngelScript execution is not stopped");
        Task<DebugMessage> response = WaitForMessageAsync(18, 3000, cancellationToken);
        using MemoryStream payload = new();
        WriteString(payload, path);
        await SendAsync(17, payload.ToArray(), cancellationToken).ConfigureAwait(false);
        _ = await response.ConfigureAwait(false);
        lock (_stateLock) return _variables.ToArray();
    }

    public async Task<DebugVariable?> EvaluateAsync(string expression, int frame, CancellationToken cancellationToken)
    {
        if (!IsStopped) throw new InvalidOperationException("AngelScript execution is not stopped");
        Task<DebugMessage> response = WaitForMessageAsync(20, 3000, cancellationToken);
        using MemoryStream payload = new();
        WriteString(payload, expression);
        WriteInt32(payload, frame);
        await SendAsync(19, payload.ToArray(), cancellationToken).ConfigureAwait(false);
        _ = await response.ConfigureAwait(false);
        lock (_stateLock) return _evaluation;
    }

    private async Task ConnectLoopAsync(CancellationToken cancellationToken)
    {
        bool hasLoggedConnectionFailure = false;
        while (!cancellationToken.IsCancellationRequested)
        {
            TcpClient client = new();
            try
            {
                await client.ConnectAsync(_host, _port, cancellationToken).ConfigureAwait(false);
                hasLoggedConnectionFailure = false;
                NetworkStream stream = client.GetStream();
                lock (_stateLock)
                {
                    _client = client;
                    _stream = stream;
                    _startedDebugging = false;
                    _incomingDatabase = new JsonObject();
                    _databaseReady = false;
                    _diagnostics.Clear();
                    _stack.Clear();
                    _variables.Clear();
                    _evaluation = null;
                    _serverVersion = 0;
                    _lastStopReason = null;
                    _lastStopDescription = null;
                    _lastStopText = null;
                }
                await SendAsync(1, Array.Empty<byte>(), cancellationToken).ConfigureAwait(false);
                await ReadLoopAsync(stream, cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested) { }
            catch (Exception exception) when (exception is IOException or SocketException or ObjectDisposedException or InvalidDataException or JsonException)
            {
                if (!hasLoggedConnectionFailure)
                {
                    string reason = exception is InvalidDataException invalidData ? invalidData.Message : exception.GetType().Name;
                    Console.Error.WriteLine($"DebugServer connection unavailable: {reason}");
                    hasLoggedConnectionFailure = true;
                }
            }
            finally
            {
                bool wasConnected = false;
                lock (_stateLock)
                {
                    if (ReferenceEquals(_client, client))
                    {
                        wasConnected = true;
                        _stream = null;
                        _client = null;
                        _startedDebugging = false;
                    }
                }
                client.Dispose();
                if (wasConnected) ConnectionLost?.Invoke();
            }
            if (!cancellationToken.IsCancellationRequested)
            {
                try { await Task.Delay(2000, cancellationToken).ConfigureAwait(false); }
                catch (OperationCanceledException) { }
            }
        }
    }

    private async Task ReadLoopAsync(NetworkStream stream, CancellationToken cancellationToken)
    {
        byte[] header = new byte[5];
        while (!cancellationToken.IsCancellationRequested)
        {
            await ReadExactlyAsync(stream, header, cancellationToken).ConfigureAwait(false);
            int bodyLength = BinaryPrimitives.ReadInt32LittleEndian(header.AsSpan(0, 4));
            if (bodyLength < 0 || bodyLength > MaximumPacketBytes) throw new InvalidDataException("DebugServer packet size was invalid");
            byte[] body = new byte[bodyLength];
            await ReadExactlyAsync(stream, body, cancellationToken).ConfigureAwait(false);
            var message = new DebugMessage(header[4], body);
            HandleMessage(message);
            if (_waiters.TryGetValue(message.Type, out ConcurrentQueue<TaskCompletionSource<DebugMessage>>? queue))
            {
                while (queue.TryDequeue(out TaskCompletionSource<DebugMessage>? waiter))
                {
                    if (waiter.TrySetResult(message)) break;
                }
            }
        }
    }

    private void HandleMessage(DebugMessage message)
    {
        BinaryReader reader = new(new MemoryStream(message.Body), Encoding.UTF8, leaveOpen: false);
        lock (_stateLock)
        {
            switch (message.Type)
            {
                case 0:
                    string file = ReadString(reader);
                    int count = ReadCount(reader);
                    List<DiagnosticRecord> fileDiagnostics = new(count);
                    for (int i = 0; i < count; ++i)
                        fileDiagnostics.Add(new DiagnosticRecord(file, ReadString(reader), reader.ReadInt32(), reader.ReadInt32(), ReadBool(reader), ReadBool(reader)));
                    _diagnostics.RemoveAll(item => string.Equals(item.File, file, StringComparison.OrdinalIgnoreCase));
                    _diagnostics.AddRange(fileDiagnostics);
                    break;
                case 2:
                    string json = ReadString(reader);
                    JsonObject? chunk = JsonNode.Parse(json)?.AsObject();
                    if (chunk is not null) foreach ((string key, JsonNode? value) in chunk) _incomingDatabase[key] = value?.DeepClone();
                    break;
                case 8:
                    int frameCount = ReadCount(reader);
                    List<CallFrame> frames = new(frameCount);
                    for (int i = 0; i < frameCount; ++i)
                    {
                        string name = ReadString(reader);
                        string source = ReadString(reader);
                        int line = reader.ReadInt32();
                        string module = _serverVersion > 0 ? ReadString(reader) : string.Empty;
                        frames.Add(new CallFrame(name, source, line, module));
                    }
                    _stack = frames;
                    break;
                case 11:
                    _lastStopReason = ReadString(reader);
                    _lastStopDescription = ReadString(reader);
                    _lastStopText = ReadString(reader);
                    break;
                case 12:
                    _lastStopReason = null;
                    _lastStopDescription = null;
                    _lastStopText = null;
                    break;
                case 18:
                    _variables = ReadVariables(reader);
                    break;
                case 20:
                    _evaluation = ReadVariable(reader);
                    break;
                case 26:
                    _database = (JsonObject)_incomingDatabase.DeepClone();
                    _databaseReady = true;
                    SaveSnapshotLocked();
                    break;
                case 31:
                    _incomingDatabase = new JsonObject();
                    _settings = ReadSettings(reader);
                    _diagnostics.Clear();
                    _databaseReady = false;
                    break;
                case 33:
                    _serverVersion = reader.ReadInt32();
                    break;
            }
        }
    }

    private void LoadSnapshot()
    {
        try
        {
            using JsonDocument document = JsonDocument.Parse(File.ReadAllBytes(File.Exists(_snapshotPath) ? _snapshotPath : _legacySnapshotPath ?? _snapshotPath), new JsonDocumentOptions { MaxDepth = 128 });
            JsonElement root = document.RootElement;
            if (root.GetProperty("schema").GetString() != "angelscript-debug-database-v1") return;
            JsonObject database = JsonNode.Parse(root.GetProperty("database").GetRawText())?.AsObject() ?? new JsonObject();
            JsonObject settings = JsonNode.Parse(root.GetProperty("settings").GetRawText())?.AsObject() ?? new JsonObject();
            // Keep this snapshot usable while offline, but wait for the current Editor session's
            // DebugDatabaseFinished message before serving API queries when a server connects.
            lock (_stateLock) { _database = database; _settings = settings; _databaseReady = false; }
        }
        catch (Exception exception) when (exception is IOException or JsonException or KeyNotFoundException or InvalidOperationException)
        {
        }
    }

    private void SaveSnapshotLocked()
    {
        try
        {
            string? directory = Path.GetDirectoryName(_snapshotPath);
            if (directory is null) return;
            Directory.CreateDirectory(directory);
            JsonObject envelope = new()
            {
                ["schema"] = "angelscript-debug-database-v1",
                ["capturedAtUtc"] = DateTimeOffset.UtcNow.ToString("O"),
                ["database"] = _database?.DeepClone(),
                ["settings"] = _settings.DeepClone(),
            };
            string temporary = _snapshotPath + ".tmp";
            File.WriteAllText(temporary, envelope.ToJsonString(new JsonSerializerOptions { WriteIndented = false }), new UTF8Encoding(false));
            File.Move(temporary, _snapshotPath, overwrite: true);
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or JsonException)
        {
            Console.Error.WriteLine("DebugServer API snapshot could not be persisted");
        }
    }

    private static JsonObject ReadSettings(BinaryReader reader)
    {
        int version = reader.ReadInt32();
        string[] names = { "automaticImports", "floatIsFloat64", "useAngelscriptHaze", "deprecateStaticClass", "disallowStaticClass", "exposeGlobalFunctions", "deprecateActorGenerics", "disallowActorGenerics" };
        JsonObject settings = new() { ["version"] = version };
        foreach (string name in names) settings[name] = ReadBool(reader);
        return settings;
    }

    private List<DebugVariable> ReadVariables(BinaryReader reader)
    {
        int count = ReadCount(reader);
        List<DebugVariable> result = new(count);
        for (int i = 0; i < count; ++i) result.Add(ReadVariable(reader));
        return result;
    }

    private DebugVariable ReadVariable(BinaryReader reader)
    {
        string name = ReadString(reader);
        string value = ReadString(reader);
        string type = ReadString(reader);
        bool members = ReadBool(reader);
        ulong address = 0;
        byte size = 0;
        if (_serverVersion >= 2) { address = reader.ReadUInt64(); size = reader.ReadByte(); }
        return new DebugVariable(name, value, type, members, address, size);
    }

    private static int ReadCount(BinaryReader reader)
    {
        int value = reader.ReadInt32();
        if (value < 0 || value > MaximumCollectionEntries) throw new InvalidDataException("DebugServer collection size was invalid");
        return value;
    }

    private static bool ReadBool(BinaryReader reader) => reader.ReadInt32() != 0;

    private static string ReadString(BinaryReader reader)
    {
        int count = reader.ReadInt32();
        if (count == int.MinValue) throw new InvalidDataException("DebugServer string size was invalid");
        int byteCount = count < 0 ? checked(-count * 2) : count;
        if (byteCount > MaximumPacketBytes) throw new InvalidDataException("DebugServer string size was invalid");
        string value;
        if (count < 0)
        {
            byte[] bytes = reader.ReadBytes(byteCount);
            if (bytes.Length != byteCount) throw new EndOfStreamException();
            value = Encoding.Unicode.GetString(bytes);
        }
        else
        {
            byte[] bytes = reader.ReadBytes(byteCount);
            if (bytes.Length != byteCount) throw new EndOfStreamException();
            value = Encoding.UTF8.GetString(bytes);
        }
        return value.EndsWith('\0') ? value[..^1] : value;
    }

    private static void WriteString(Stream stream, string value)
    {
        bool ascii = value.All(character => character <= 0x7f);
        if (ascii)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(value + "\0");
            WriteInt32(stream, bytes.Length);
            stream.Write(bytes);
        }
        else
        {
            byte[] bytes = Encoding.Unicode.GetBytes(value + "\0");
            WriteInt32(stream, -bytes.Length / 2);
            stream.Write(bytes);
        }
    }

    private static byte[] WriteInt32(int value)
    {
        byte[] bytes = new byte[4];
        BinaryPrimitives.WriteInt32LittleEndian(bytes, value);
        return bytes;
    }

    private static void WriteInt32(Stream stream, int value) => stream.Write(WriteInt32(value));

    private static async Task ReadExactlyAsync(Stream stream, Memory<byte> buffer, CancellationToken cancellationToken)
    {
        int offset = 0;
        while (offset < buffer.Length)
        {
            int read = await stream.ReadAsync(buffer[offset..], cancellationToken).ConfigureAwait(false);
            if (read == 0) throw new EndOfStreamException();
            offset += read;
        }
    }

    public async ValueTask DisposeAsync()
    {
        _shutdown.Cancel();
        lock (_stateLock) { _stream?.Dispose(); _client?.Dispose(); _stream = null; _client = null; }
        try { await _connectionTask.WaitAsync(TimeSpan.FromSeconds(3)).ConfigureAwait(false); }
        catch (Exception exception) when (exception is OperationCanceledException or TimeoutException or SocketException or ObjectDisposedException) { }
        _shutdown.Dispose();
        _sendLock.Dispose();
    }
}

internal sealed record DebugMessage(byte Type, byte[] Body);
internal sealed record DiagnosticRecord(string File, string Message, int Line, int Character, bool IsError, bool IsInfo);
internal sealed record CallFrame(string Name, string Source, int Line, string Module);
internal sealed record DebugVariable(string Name, string Value, string Type, bool HasMembers, ulong Address, byte Size);
