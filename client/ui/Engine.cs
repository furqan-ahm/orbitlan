using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace OrbitLan;

public sealed class PeerModel
{
    [JsonPropertyName("name")] public string Name { get; set; } = "";
    [JsonPropertyName("ip")] public string IP { get; set; } = "";
    [JsonPropertyName("state")] public string State { get; set; } = "";
    [JsonPropertyName("rttMs")] public double RttMs { get; set; }
}

public sealed class StatusModel
{
    [JsonPropertyName("myID")] public string MyID { get; set; } = "";
    [JsonPropertyName("myName")] public string MyName { get; set; } = "";
    [JsonPropertyName("myIP")] public string MyIP { get; set; } = "";
    [JsonPropertyName("code")] public string Code { get; set; } = "";
    [JsonPropertyName("peers")] public List<PeerModel> Peers { get; set; } = new();
    [JsonPropertyName("summary")] public string Summary { get; set; } = "";
}

public sealed class Engine
{
    public static readonly Engine Instance = new();

    public event Action<StatusModel?>? StatusChanged;
    public event Action<string>? Failed;

    public bool Running => proc is { HasExited: false };
    public string LastError { get; private set; } = "";

    const string ApiBase = "http://127.0.0.1:9099";
    static readonly string PayloadDir = Path.Combine(AppContext.BaseDirectory, "payload");
    static readonly string DriverDir = Path.Combine(PayloadDir, "driver");
    static string EnginePath => Path.Combine(PayloadDir, "orbitlan-engine.exe");
    static string DevconPath => Path.Combine(DriverDir, "devcon.exe");

    readonly HttpClient http = new() { Timeout = TimeSpan.FromSeconds(4) };
    Process? proc;
    CancellationTokenSource? pollCts;
    readonly StringBuilder log = new();

    // Overall connect timeout (adapter setup + engine launch + API readiness). Standard-ish 30s.
    const int ConnectTimeoutSeconds = 30;

    public async Task<bool> StartAsync(string name, string code, string coordinatorUrl, string relayMode)
    {
        LastError = "";
        log.Clear();

        if (!Uri.TryCreate(coordinatorUrl, UriKind.Absolute, out var coordinator) ||
            (coordinator.Scheme != Uri.UriSchemeHttps && coordinator.Scheme != Uri.UriSchemeHttp))
        {
            Fail("Enter a valid coordinator URL in Settings.");
            return false;
        }
        relayMode = relayMode.ToLowerInvariant();
        if (relayMode is not ("off" or "auto" or "on"))
        {
            Fail("Relay mode must be Off, Auto, or On.");
            return false;
        }

        if (!File.Exists(EnginePath))
        {
            Fail($"engine not found at {EnginePath}");
            return false;
        }

        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(ConnectTimeoutSeconds));
        var ct = cts.Token;

        // All the blocking bits (devcon/netsh adapter setup, process launch) run OFF the UI thread
        // so the window/cursor never freeze. Returns an error string, or null on success.
        string? startErr = await Task.Run(() =>
        {
            if (!AdapterPresent())
            {
                var (ok, msg) = InstallAdapter();
                if (!ok) return "Network adapter setup failed — " + msg;
                Thread.Sleep(1500); // let Windows settle the new adapter
            }
            var psi = new ProcessStartInfo
            {
                FileName = EnginePath,
                WorkingDirectory = PayloadDir,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardError = true,
                RedirectStandardOutput = true,
            };
            psi.ArgumentList.Add("-code"); psi.ArgumentList.Add(code);
            psi.ArgumentList.Add("-name"); psi.ArgumentList.Add(name);
            psi.ArgumentList.Add("-server"); psi.ArgumentList.Add(coordinatorUrl.TrimEnd('/'));
            psi.ArgumentList.Add("-relay"); psi.ArgumentList.Add(relayMode);
            psi.ArgumentList.Add("-datapath"); psi.ArgumentList.Add("tap");
            psi.ArgumentList.Add("-api"); psi.ArgumentList.Add("127.0.0.1:9099");
            try
            {
                var p = Process.Start(psi)!;
                proc = p;
                p.ErrorDataReceived += (_, e) => { if (e.Data != null) lock (log) log.AppendLine(e.Data); };
                p.OutputDataReceived += (_, e) => { if (e.Data != null) lock (log) log.AppendLine(e.Data); };
                p.BeginErrorReadLine();
                p.BeginOutputReadLine();
            }
            catch (Exception ex) { return "could not start engine: " + ex.Message; }
            return null;
        }, ct);

        if (startErr != null) { Stop(); Fail(startErr); return false; }

        // wait for the API to come up, or the process to die, until the timeout
        try
        {
            while (!ct.IsCancellationRequested)
            {
                if (proc is { HasExited: true })
                {
                    Fail(FirstError() ?? "engine exited — is the TAP driver installed?");
                    return false;
                }
                try
                {
                    using var r = await http.GetAsync(ApiBase + "/health", ct);
                    if (r.IsSuccessStatusCode) { StartPolling(); return true; }
                }
                catch (OperationCanceledException) { break; }
                catch { /* not up yet */ }
                await Task.Delay(250, ct);
            }
        }
        catch (OperationCanceledException) { /* timed out */ }

        Stop();
        Fail($"Connection timed out after {ConnectTimeoutSeconds}s.");
        return false;
    }

    void StartPolling()
    {
        pollCts?.Cancel();
        var cts = new CancellationTokenSource();
        pollCts = cts;
        _ = Task.Run(async () =>
        {
            while (!cts.IsCancellationRequested)
            {
                try
                {
                    var json = await http.GetStringAsync(ApiBase + "/status", cts.Token);
                    var st = JsonSerializer.Deserialize<StatusModel>(json);
                    StatusChanged?.Invoke(st);
                }
                catch { }
                try { await Task.Delay(1000, cts.Token); } catch { break; }
                if (proc is { HasExited: true }) { Fail(FirstError() ?? "engine stopped"); break; }
            }
        }, cts.Token);
    }

    public void Stop()
    {
        pollCts?.Cancel();
        pollCts = null;
        var p = proc;
        proc = null;
        if (p != null)
        {
            try { if (!p.HasExited) p.Kill(entireProcessTree: true); } catch { }
            try { p.Dispose(); } catch { }
        }
        StatusChanged?.Invoke(null);
    }

    // ---- adapter / driver helpers (devcon: installs driver + creates the tap0901 adapter) ----

    public bool AdapterPresent()
    {
        // Fast path: scan the network-adapter class in the registry for a tap0901 instance.
        // (Spawning `devcon status` just to check would add ~1-2s to every connect.)
        try
        {
            using var cls = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(
                @"SYSTEM\CurrentControlSet\Control\Class\{4D36E972-E325-11CE-BFC1-08002BE10318}");
            if (cls == null) return false;
            foreach (var sub in cls.GetSubKeyNames())
            {
                using var k = cls.OpenSubKey(sub);
                if ((k?.GetValue("ComponentId") as string)?.Equals("tap0901", StringComparison.OrdinalIgnoreCase) == true)
                    return true;
            }
            return false;
        }
        catch { return true; } // can't read -> assume present; the engine reports clearly if not
    }

    // InstallAdapter installs the signed driver and creates one adapter. Returns (ok, message).
    // May pop a one-time Windows "install this device software?" trust prompt.
    public (bool ok, string msg) InstallAdapter()
    {
        if (!File.Exists(DevconPath))
            return (false, "driver files missing from payload");
        var (ok, outp) = RunCapture(DevconPath, "install OemVista.inf tap0901", DriverDir);
        if (!ok)
            return (false, "driver/adapter install failed: " + Trim(outp));
        return (true, "adapter created");
    }

    static void EnsureAdapter()
    {
        // best-effort; the public InstallAdapter path (driven by the UI) is the primary one
    }

    static (bool, string) RunCapture(string exe, string args, string? cwd = null)
    {
        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = exe, Arguments = args,
                UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true,
            };
            if (cwd != null) psi.WorkingDirectory = cwd;
            var p = Process.Start(psi)!;
            string o = p.StandardOutput.ReadToEnd() + p.StandardError.ReadToEnd();
            p.WaitForExit(30000);
            return (p.ExitCode == 0, o);
        }
        catch (Exception ex) { return (false, ex.Message); }
    }

    static string Trim(string s) => s.Length > 200 ? s[..200] : s;

    // SetAdapterPriority raises/restores the OrbitLan adapter's interface metric. Priority ON
    // (metric 1) makes stubborn/older games broadcast LAN discovery out the virtual adapter;
    // it's safe (the adapter has no gateway, so internet routing is unaffected) and fully
    // reversible (metric=automatic restores Windows' default).
    public static bool SetAdapterPriority(bool prioritize)
    {
        string metric = prioritize ? "1" : "automatic";
        var (ok4, _) = RunCapture("netsh", $"interface ipv4 set interface \"OrbitLan\" metric={metric}");
        return ok4;
    }

    string? FirstError()
    {
        foreach (var line in log.ToString().Split('\n'))
            if (line.Contains("failed", StringComparison.OrdinalIgnoreCase) ||
                line.Contains("FATAL", StringComparison.OrdinalIgnoreCase) ||
                line.Contains("error", StringComparison.OrdinalIgnoreCase))
                return line.Trim();
        return null;
    }

    void Fail(string msg)
    {
        LastError = msg;
        Failed?.Invoke(msg);
    }
}
