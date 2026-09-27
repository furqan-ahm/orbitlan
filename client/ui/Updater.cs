using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace OrbitLan;

/// <summary>
/// Signed, self-contained auto-update. Safety model:
///   • The manifest carries ONLY version + url + sha256 + an RSA signature. Never scripts/commands.
///   • The signature (over "version\nurl\nsha256") is verified with a baked-in public key BEFORE
///     anything is trusted — so a tampered HTTP channel can't push a fake update.
///   • The downloaded zip is SHA-256 verified against the signed hash before it's applied.
///   • Apply is download → verify → stage → swap-after-exit → relaunch; the running install is
///     never touched until a verified package is staged.
/// One-time corrective actions (e.g. adapter fixes) are NOT fetched-and-run — they live in
/// Migrations.cs (compiled-in code, run once by id). An update carries a fix by shipping new code.
/// </summary>
public sealed class Updater
{
    // RSA-2048 public update-signing key. Private half never leaves the dev machine.
    const string PubKeyPem =
        "-----BEGIN PUBLIC KEY-----\n" +
        "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAtSdOQUHQrv+xqZp/biRC\n" +
        "8oNrK6rM9QATDCKh5hejqXx0jwCLqlkC5FfLntbQD5rW5NEKYCXVcC980o1UJub/\n" +
        "M6I8q8U4+lkQj6K+V8zQA5qDY47BVnIzIyili5ZDVcMYD4FFrFII0G5P+xsGP3oc\n" +
        "6T7Wz79aTnK/bjNmXwokBX59OFr1CnDffTyQcNQ0vn6LkIzGjCyQqCjWzm9lHWmM\n" +
        "1E1ZnDyLTN8I/wNPNhgjQEnR0UoVwG2tDyuB1daFUzwm2qnN6CDUbpM/7qgvnA0a\n" +
        "iCzLp9RwjlrkAvk5xgW6eSEesz38vftL7A0fjFAoljIwFNi2Qz+jHqgV+2yyhfFi\n" +
        "7wIDAQAB\n" +
        "-----END PUBLIC KEY-----";

    public sealed class UpdateInfo
    {
        public string Version = "", Url = "", Sha256 = "", Notes = "";
    }

    public static Version Current =>
        typeof(Updater).Assembly.GetName().Version ?? new Version(1, 0, 0);

    readonly HttpClient http = new() { Timeout = TimeSpan.FromSeconds(20) };

    /// <summary>Returns a verified, newer update, or null (on any error / bad signature / same version).</summary>
    public async Task<UpdateInfo?> CheckAsync()
    {
        try
        {
            var json = await http.GetStringAsync(ProductDefaults.UpdateManifestUrl);
            var root = JsonDocument.Parse(json).RootElement;
            string version = root.GetProperty("version").GetString() ?? "";
            string url = root.GetProperty("url").GetString() ?? "";
            string sha = (root.GetProperty("sha256").GetString() ?? "").ToLowerInvariant();
            string notes = root.TryGetProperty("notes", out var nEl) ? (nEl.GetString() ?? "") : "";
            string sigB64 = root.GetProperty("sig").GetString() ?? "";

            // verify signature over the exact canonical payload
            string payload = $"{version}\n{url}\n{sha}";
            if (!VerifySig(payload, sigB64))
                return null; // NEVER trust an unsigned/forged manifest

            if (!Version.TryParse(version, out var v) || Norm(v) <= Norm(Current))
                return null;

            return new UpdateInfo { Version = version, Url = url, Sha256 = sha, Notes = notes };
        }
        catch { return null; } // network down / no manifest / parse error -> just no update
    }

    // normalize to major.minor.build so a 3-part manifest ("1.3.0") compares cleanly against a
    // 4-part assembly version ("1.2.0.0") without the unspecified-revision (-1) quirk.
    static Version Norm(Version x) => new(x.Major, x.Minor, Math.Max(0, x.Build));

    static bool VerifySig(string payload, string sigB64)
    {
        try
        {
            using var rsa = RSA.Create();
            rsa.ImportFromPem(PubKeyPem);
            return rsa.VerifyData(Encoding.UTF8.GetBytes(payload), Convert.FromBase64String(sigB64),
                HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
        }
        catch { return false; }
    }

    /// <summary>
    /// Downloads + verifies the update, stages it, and launches a helper that swaps files once this
    /// process exits and relaunches. Returns true if the swap was launched (caller should then exit).
    /// </summary>
    public async Task<bool> ApplyAsync(UpdateInfo info, Action<string> progress)
    {
        string tmp = Path.Combine(Path.GetTempPath(), "OrbitLan-update");
        try { if (Directory.Exists(tmp)) Directory.Delete(tmp, true); } catch { }
        Directory.CreateDirectory(tmp);
        string zipPath = Path.Combine(tmp, "update.zip");

        progress("Downloading…");
        using (var resp = await http.GetAsync(info.Url, HttpCompletionOption.ResponseHeadersRead))
        {
            resp.EnsureSuccessStatusCode();
            await using var fs = File.Create(zipPath);
            await resp.Content.CopyToAsync(fs);
        }

        progress("Verifying…");
        string got = Convert.ToHexString(SHA256.HashData(await File.ReadAllBytesAsync(zipPath))).ToLowerInvariant();
        if (got != info.Sha256)
        {
            progress("Verification FAILED — update rejected.");
            return false; // hash mismatch: refuse to apply
        }

        progress("Preparing…");
        string stageRoot = Path.Combine(tmp, "stage");
        Directory.CreateDirectory(stageRoot);
        ZipFile.ExtractToDirectory(zipPath, stageRoot);
        // zip root is "OrbitLan/…"; find the folder that contains OrbitLan.exe
        string stage = FindExeDir(stageRoot) ?? stageRoot;

        string installDir = AppContext.BaseDirectory.TrimEnd('\\');
        int pid = Environment.ProcessId;
        string bat = Path.Combine(tmp, "apply-update.bat");
        await File.WriteAllTextAsync(bat, BuildUpdaterBat(pid, stage, installDir, tmp));

        Process.Start(new ProcessStartInfo
        {
            FileName = "cmd.exe",
            Arguments = $"/c \"{bat}\"",
            UseShellExecute = false,
            CreateNoWindow = true,
        });
        progress("Restarting to finish update…");
        return true;
    }

    static string? FindExeDir(string root)
    {
        foreach (var f in Directory.EnumerateFiles(root, "OrbitLan.exe", SearchOption.AllDirectories))
            return Path.GetDirectoryName(f);
        return null;
    }

    // Waits for our PID to exit (so files unlock), copies the staged build over the install dir,
    // relaunches, then cleans up. robocopy /IS/IT overwrites; exit codes < 8 are success.
    static string BuildUpdaterBat(int pid, string stage, string installDir, string tmp) =>
        "@echo off\r\n" +
        "setlocal\r\n" +
        ":wait\r\n" +
        $"tasklist /FI \"PID eq {pid}\" 2>nul | find \"{pid}\" >nul\r\n" +
        "if not errorlevel 1 ( timeout /t 1 /nobreak >nul & goto wait )\r\n" +
        "timeout /t 1 /nobreak >nul\r\n" +
        $"robocopy \"{stage}\" \"{installDir}\" /E /IS /IT /R:3 /W:1 /NFL /NDL /NJH /NJS >nul\r\n" +
        $"start \"\" \"{installDir}\\OrbitLan.exe\"\r\n" +
        $"rmdir /s /q \"{tmp}\" >nul 2>&1\r\n";
}
