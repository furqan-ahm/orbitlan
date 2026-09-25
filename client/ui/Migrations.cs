using System.Diagnostics;

namespace OrbitLan;

/// <summary>
/// One-time corrective actions that run exactly once per machine, ever (tracked by id in prefs).
/// This is how an update carries a fix — you add a migration here, ship a new signed build, and
/// clients run the new ids once on next launch. It is deliberately NOT remote code: every action
/// is compiled-in and reviewed. Keep each action idempotent and scoped (e.g. adapter/firewall).
/// </summary>
public static class Migrations
{
    // APPEND-ONLY. Never edit or reorder existing ids. Add new fixes at the bottom.
    static readonly (string Id, Action Run)[] All =
    {
        // v1.2.0 — ensure the virtual-LAN firewall allow rule exists even on machines whose engine
        // never wrote it. Idempotent and scoped strictly to the 10.69.0.0/24 overlay subnet.
        ("2026-10-ensure-lan-firewall", () =>
        {
            Netsh("advfirewall firewall delete rule name=OrbitLan");
            Netsh("advfirewall firewall add rule name=OrbitLan dir=in action=allow remoteip=10.69.0.0/24");
        }),
        // Remove the firewall rule left by builds released under the previous product name.
        ("2026-10-remove-previous-brand-firewall", () =>
        {
            Netsh("advfirewall firewall delete rule name=OrbitVLAN");
        }),
    };

    /// <summary>Runs any migration whose id isn't in `already`; calls markDone(id) after each.</summary>
    public static void RunPending(ISet<string> already, Action<string> markDone)
    {
        foreach (var (id, run) in All)
        {
            if (already.Contains(id)) continue;
            try { run(); } catch { /* best-effort; still mark done so it can't loop */ }
            markDone(id);
        }
    }

    static void Netsh(string args)
    {
        try
        {
            var p = Process.Start(new ProcessStartInfo
            {
                FileName = "netsh", Arguments = args, UseShellExecute = false, CreateNoWindow = true,
            });
            p?.WaitForExit(15000);
        }
        catch { }
    }
}
