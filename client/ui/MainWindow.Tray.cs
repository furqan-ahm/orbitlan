using Forms = System.Windows.Forms;

namespace OrbitLan;

public partial class MainWindow
{
    Forms.NotifyIcon? trayIcon;
    Forms.ToolStripMenuItem? trayDisconnectItem;
    bool trayHintShown;

    void InitializeTray()
    {
        var menu = new Forms.ContextMenuStrip();
        var open = new Forms.ToolStripMenuItem("Open OrbitLan");
        trayDisconnectItem = new Forms.ToolStripMenuItem("Disconnect") { Enabled = false };
        var exit = new Forms.ToolStripMenuItem("Exit");

        open.Click += (_, _) => Dispatcher.Invoke(RestoreFromTray);
        trayDisconnectItem.Click += (_, _) => Dispatcher.Invoke(Disconnect);
        exit.Click += (_, _) => Dispatcher.Invoke(Close);
        menu.Items.Add(open);
        menu.Items.Add(trayDisconnectItem);
        menu.Items.Add(new Forms.ToolStripSeparator());
        menu.Items.Add(exit);

        var extracted = System.Drawing.Icon.ExtractAssociatedIcon(Environment.ProcessPath!);
        trayIcon = new Forms.NotifyIcon
        {
            Icon = extracted == null ? System.Drawing.SystemIcons.Application : (System.Drawing.Icon)extracted.Clone(),
            Text = "OrbitLan — Offline",
            Visible = true,
            ContextMenuStrip = menu,
        };
        extracted?.Dispose();
        trayIcon.DoubleClick += (_, _) => Dispatcher.Invoke(RestoreFromTray);
        StateChanged += (_, _) =>
        {
            if (WindowState == System.Windows.WindowState.Minimized) HideToTray();
        };
    }

    void HideToTray()
    {
        Hide();
        ShowInTaskbar = false;
        if (!trayHintShown && trayIcon != null)
        {
            trayHintShown = true;
            trayIcon.ShowBalloonTip(2200, "OrbitLan is still running",
                "Open it from the notification area. Your LAN connection stays active.",
                Forms.ToolTipIcon.Info);
        }
    }

    void RestoreFromTray()
    {
        ShowInTaskbar = true;
        Show();
        WindowState = System.Windows.WindowState.Normal;
        Activate();
        Topmost = true;
        Topmost = false;
        Focus();
    }

    void UpdateTrayStatus(string status)
    {
        if (trayIcon == null) return;
        trayIcon.Text = $"OrbitLan — {status}";
        if (trayDisconnectItem != null)
            trayDisconnectItem.Enabled = status == "Connected";
    }

    void DisposeTray()
    {
        if (trayIcon == null) return;
        trayIcon.Visible = false;
        trayIcon.Dispose();
        trayIcon = null;
    }
}
