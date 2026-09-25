using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Shapes;

namespace OrbitLan;

public sealed class PeerRow : INotifyPropertyChanged
{
    public string Name { get; set; } = "";
    string _ip = "", _label = "", _rtt = "";
    Brush _brush = Brushes.Gray;

    public string IP { get => _ip; set { _ip = value; OnCh(nameof(IP)); } }
    public string StatusLabel { get => _label; set { _label = value; OnCh(nameof(StatusLabel)); } }
    public string RttText { get => _rtt; set { _rtt = value; OnCh(nameof(RttText)); } }
    public Brush StatusBrush { get => _brush; set { _brush = value; OnCh(nameof(StatusBrush)); } }

    public event PropertyChangedEventHandler? PropertyChanged;
    void OnCh(string n) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(n));
}

public partial class MainWindow : Window
{
    static readonly Brush Good = Freeze(0x4A, 0xDE, 0x80);
    static readonly Brush Amber = Freeze(0xF5, 0xC4, 0x4A);
    static readonly Brush Dim = Freeze(0x8B, 0x93, 0xB8);
    static readonly Brush Accent = Freeze(0x6C, 0x8C, 0xFF);

    readonly ObservableCollection<PeerRow> rows = new();
    readonly Engine engine = Engine.Instance;
    readonly Updater updater = new();
    Updater.UpdateInfo? _pendingUpdate;
    bool _priority;
    string _coordinatorUrl = ProductDefaults.CoordinatorUrl;
    string _relayMode = "auto";
    readonly HashSet<string> _ranMigrations = new();

    static readonly string ConfigPath = System.IO.Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "OrbitLan", "config.json");
    static readonly string PreviousConfigPath = System.IO.Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "OrbitVLAN", "config.json");
    static readonly string LegacyPrefsPath = System.IO.Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "OrbitVLAN", "ui.json");

    public MainWindow()
    {
        InitializeComponent();
        MembersList.ItemsSource = rows;
        NameBox.Text = Environment.UserName;
        CodeBox.Text = RandomCode();
        SpawnStars();
        LoadPrefs();
        UpdatePriorityUI();
        SupportBtn.IsEnabled = !string.IsNullOrWhiteSpace(ProductDefaults.SupportUrl);
        var v = Updater.Current;
        VersionText.Text = $"OrbitLan v{v.Major}.{v.Minor}.{Math.Max(0, v.Build)}";
        engine.StatusChanged += st => Dispatcher.Invoke(() => OnStatus(st));
        engine.Failed += msg => Dispatcher.Invoke(() => OnFail(msg));

        // one-time corrective actions (compiled-in, run once by id)
        Migrations.RunPending(_ranMigrations, id => { _ranMigrations.Add(id); SavePrefs(); });
        // check for a signed update in the background
        _ = CheckForUpdateAsync();
    }

    // ---- auto-update ----

    async Task CheckForUpdateAsync()
    {
        var info = await updater.CheckAsync();
        if (info == null) return;
        Dispatcher.Invoke(() =>
        {
            _pendingUpdate = info;
            UpdateText.Text = $"Update available — v{info.Version}";
            UpdateSub.Text = string.IsNullOrWhiteSpace(info.Notes) ? "Click to update and restart." : info.Notes;
            UpdateBanner.Visibility = Visibility.Visible;
        });
    }

    async void Update_Click(object sender, RoutedEventArgs e)
    {
        if (_pendingUpdate == null) return;
        UpdateBtn.IsEnabled = false;
        bool ok = await updater.ApplyAsync(_pendingUpdate, s => Dispatcher.Invoke(() => UpdateSub.Text = s));
        if (ok)
        {
            engine.Stop();
            Application.Current.Shutdown(); // helper swaps files + relaunches once we exit
        }
        else
        {
            UpdateBtn.IsEnabled = true;
        }
    }

    // ---- settings ----

    void Settings_Click(object sender, MouseButtonEventArgs e)
    {
        e.Handled = true;
        UpdatePriorityUI();
        CoordinatorBox.Text = _coordinatorUrl;
        RelayModeBox.SelectedValue = _relayMode;
        SettingsError.Text = "";
        SettingsOverlay.Visibility = Visibility.Visible;
    }

    void SettingsClose_Click(object sender, MouseButtonEventArgs e)
    {
        e.Handled = true;
        SettingsOverlay.Visibility = Visibility.Collapsed;
    }

    void SettingsClose_Click(object sender, RoutedEventArgs e) => SettingsOverlay.Visibility = Visibility.Collapsed;

    void Swallow(object sender, MouseButtonEventArgs e) => e.Handled = true; // clicks on the card don't close it

    void PriorityToggle_Click(object sender, RoutedEventArgs e)
    {
        bool target = !_priority;
        if (Engine.SetAdapterPriority(target))
        {
            _priority = target;
            SavePrefs();
            UpdatePriorityUI();
            PriorityStatus.Text = target
                ? "Applied — OrbitLan is now prioritized. Restart the game if it was open."
                : "Reverted to Windows default.";
        }
        else
        {
            PriorityStatus.Text = "Couldn't change it — connect first so the OrbitLan adapter exists.";
        }
    }

    void SettingsSave_Click(object sender, RoutedEventArgs e)
    {
        string coordinator = CoordinatorBox.Text.Trim().TrimEnd('/');
        if (!Uri.TryCreate(coordinator, UriKind.Absolute, out var uri) ||
            (uri.Scheme != Uri.UriSchemeHttps && uri.Scheme != Uri.UriSchemeHttp))
        {
            SettingsError.Text = "Enter a valid http:// or https:// coordinator URL.";
            return;
        }
        string relay = RelayModeBox.SelectedValue as string ?? "auto";
        if (relay is not ("off" or "auto" or "on")) relay = "auto";
        _coordinatorUrl = coordinator;
        _relayMode = relay;
        SavePrefs();
        SettingsOverlay.Visibility = Visibility.Collapsed;
    }

    void Support_Click(object sender, RoutedEventArgs e)
    {
        if (string.IsNullOrWhiteSpace(ProductDefaults.SupportUrl)) return;
        try
        {
            Process.Start(new ProcessStartInfo(ProductDefaults.SupportUrl) { UseShellExecute = true });
        }
        catch { SettingsError.Text = "Couldn't open the support page."; }
    }

    void UpdatePriorityUI()
    {
        PriorityToggle.Content = _priority ? "Turn off" : "Turn on";
        PriorityStatus.Text = _priority
            ? "Currently: ON — prioritized for older games."
            : "Currently: off (Windows default).";
    }

    void LoadPrefs()
    {
        try
        {
            string path = new[] { ConfigPath, PreviousConfigPath, LegacyPrefsPath }
                .FirstOrDefault(File.Exists) ?? ConfigPath;
            if (!File.Exists(path)) return;
            var n = JsonDocument.Parse(File.ReadAllText(path)).RootElement;
            if (n.TryGetProperty("priority", out var p)) _priority = p.GetBoolean();
            if (n.TryGetProperty("coordinatorURL", out var c) && c.GetString() is string url &&
                !string.IsNullOrWhiteSpace(url)) _coordinatorUrl = url.TrimEnd('/');
            if (n.TryGetProperty("relayMode", out var r) && r.GetString() is string relay &&
                relay is "off" or "auto" or "on") _relayMode = relay;
            if (n.TryGetProperty("ranMigrations", out var m) && m.ValueKind == JsonValueKind.Array)
                foreach (var e in m.EnumerateArray())
                    if (e.GetString() is string s) _ranMigrations.Add(s);
        }
        catch { }
    }

    void SavePrefs()
    {
        try
        {
            Directory.CreateDirectory(System.IO.Path.GetDirectoryName(ConfigPath)!);
            File.WriteAllText(ConfigPath, JsonSerializer.Serialize(new
            {
                coordinatorURL = _coordinatorUrl,
                relayMode = _relayMode,
                priority = _priority,
                ranMigrations = _ranMigrations.ToArray(),
            }));
        }
        catch { }
    }

    static Brush Freeze(byte r, byte g, byte b)
    {
        var br = new SolidColorBrush(Color.FromRgb(r, g, b));
        br.Freeze();
        return br;
    }

    // ---- actions ----

    void New_Click(object sender, RoutedEventArgs e) => CodeBox.Text = RandomCode();

    async void Connect_Click(object sender, RoutedEventArgs e)
    {
        SetupError.Text = "";
        var name = string.IsNullOrWhiteSpace(NameBox.Text) ? "player" : NameBox.Text.Trim();
        var code = CodeBox.Text.Trim().ToLowerInvariant();
        if (name.Length > 64) { SetupError.Text = "Your name must be 64 characters or fewer."; return; }
        if (code.Length is < 3 or > 64 || code.Any(c => !char.IsAsciiLetterOrDigit(c) && c is not '-' and not '_'))
        {
            SetupError.Text = "Use 3-64 letters, numbers, dashes, or underscores for the network code.";
            return;
        }

        SetStatus("Connecting", Accent);
        ConnectBtn.IsHitTestVisible = false;           // block clicks without dimming (keeps spinner bright)
        ConnectBtn.Content = SpinnerContent("Connecting…");
        bool ok = await engine.StartAsync(name, code, _coordinatorUrl, _relayMode); // fully off the UI thread
        ConnectBtn.IsHitTestVisible = true;
        ConnectBtn.Content = "Connect";
        if (ok)
        {
            CodeDisplay.Text = code;
            ShowConnected(true);
            SetStatus("Connected", Good);
        }
        else
        {
            SetStatus("Offline", Dim);
        }
    }

    // a small rotating-arc spinner + label, used as the Connect button's content while connecting
    static UIElement SpinnerContent(string text)
    {
        var arc = new System.Windows.Shapes.Path
        {
            Stroke = Brushes.White,
            StrokeThickness = 2.5,
            Width = 18,
            Height = 18,
            Data = Geometry.Parse("M 9,1 A 8,8 0 1 1 1,9"), // ~270-degree arc
            StrokeStartLineCap = PenLineCap.Round,
            StrokeEndLineCap = PenLineCap.Round,
            RenderTransformOrigin = new Point(0.5, 0.5),
            VerticalAlignment = VerticalAlignment.Center,
        };
        var rot = new RotateTransform(0);
        arc.RenderTransform = rot;
        rot.BeginAnimation(RotateTransform.AngleProperty,
            new DoubleAnimation(0, 360, TimeSpan.FromSeconds(0.85)) { RepeatBehavior = RepeatBehavior.Forever });

        var sp = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
        };
        sp.Children.Add(arc);
        sp.Children.Add(new TextBlock
        {
            Text = text, Foreground = Brushes.White, FontSize = 16, FontWeight = FontWeights.SemiBold,
            Margin = new Thickness(10, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center,
        });
        return sp;
    }

    void Disconnect_Click(object sender, RoutedEventArgs e)
    {
        engine.Stop();
        rows.Clear();
        ShowConnected(false);
        SetStatus("Offline", Dim);
    }

    void Copy_Click(object sender, RoutedEventArgs e)
    {
        try { Clipboard.SetText(CodeDisplay.Text); } catch { }
    }

    // ---- status updates ----

    void OnStatus(StatusModel? st)
    {
        if (st == null) return;
        MyIpText.Text = string.IsNullOrEmpty(st.MyIP) ? "—" : st.MyIP;
        SummaryText.Text = st.Summary;

        // sync rows to peers
        var seen = new HashSet<string>();
        foreach (var p in st.Peers)
        {
            seen.Add(p.Name + "|" + p.IP);
            var row = FindRow(p.Name, p.IP) ?? AddRow(p.Name, p.IP);
            (string label, Brush brush) = p.State switch
            {
                "direct" => ("Direct", Good),
                "relay" => ("Relay", Amber),
                "failed" => ("Retrying", Dim),
                _ => ("Connecting", Dim),
            };
            row.StatusLabel = label;
            row.StatusBrush = brush;
            row.RttText = p.RttMs >= 0.1 ? $"{p.RttMs:0.0} ms" : (p.State == "direct" || p.State == "relay" ? "<1 ms" : "");
        }
        for (int i = rows.Count - 1; i >= 0; i--)
            if (!seen.Contains(rows[i].Name + "|" + rows[i].IP)) rows.RemoveAt(i);

        EmptyHint.Visibility = rows.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    void OnFail(string msg)
    {
        if (ConnectedView.Visibility == Visibility.Visible)
        {
            // dropped while connected
            SetStatus("Offline", Dim);
            ShowConnected(false);
        }
        SetupError.Text = msg;
        SetStatus("Offline", Dim);
    }

    PeerRow? FindRow(string name, string ip)
    {
        foreach (var r in rows) if (r.Name == name && r.IP == ip) return r;
        return null;
    }

    PeerRow AddRow(string name, string ip)
    {
        var r = new PeerRow { Name = name, IP = ip, StatusLabel = "Connecting", StatusBrush = Dim };
        rows.Add(r);
        return r;
    }

    // ---- view helpers ----

    void ShowConnected(bool connected)
    {
        ConnectedView.Visibility = connected ? Visibility.Visible : Visibility.Collapsed;
        SetupView.Visibility = connected ? Visibility.Collapsed : Visibility.Visible;
    }

    void SetStatus(string text, Brush color)
    {
        StatusText.Text = text;
        StatusDot.Fill = color;
    }

    static string RandomCode()
    {
        string[] first =
        {
            "amber", "arctic", "bright", "calm", "cobalt", "cosmic", "crystal", "daring",
            "ember", "frozen", "gentle", "golden", "hidden", "lucky", "lunar", "misty",
            "neon", "nova", "polar", "quiet", "rapid", "royal", "silver", "solar",
            "stellar", "swift", "tidal", "tiny", "velvet", "violet", "wild", "young"
        };
        string[] second =
        {
            "atlas", "beacon", "comet", "dragon", "echo", "falcon", "forest", "galaxy",
            "harbor", "island", "jupiter", "lantern", "meteor", "nebula", "oasis", "orbit",
            "phoenix", "planet", "pulsar", "quasar", "rocket", "saturn", "signal", "summit",
            "thunder", "tiger", "vertex", "voyager", "whale", "willow", "zenith", "zephyr"
        };
        return $"{first[RandomNumberGenerator.GetInt32(first.Length)]}-" +
               $"{second[RandomNumberGenerator.GetInt32(second.Length)]}-" +
               RandomNumberGenerator.GetInt32(1000, 10000);
    }

    void SpawnStars()
    {
        var rnd = new Random(7);
        // spread a little beyond the window so the slow drift never reveals an empty edge
        for (int i = 0; i < 80; i++)
        {
            double a = rnd.NextDouble();
            var s = new Ellipse
            {
                Width = 1.4 + a * 2.3, Height = 1.4 + a * 2.3,
                Fill = new SolidColorBrush(Color.FromArgb((byte)(40 + a * 90), 255, 255, 255)),
            };
            System.Windows.Controls.Canvas.SetLeft(s, -30 + rnd.NextDouble() * 500);
            System.Windows.Controls.Canvas.SetTop(s, -30 + rnd.NextDouble() * 720);
            StarsCanvas.Children.Add(s);

            // twinkle on a small subset only — a few opacity animations, composited, cheap
            if (i % 8 == 0)
            {
                var tw = new DoubleAnimation(0.35, 1.0, TimeSpan.FromSeconds(2.5 + a * 3))
                {
                    AutoReverse = true, RepeatBehavior = RepeatBehavior.Forever,
                    BeginTime = TimeSpan.FromSeconds(a * 3),
                };
                s.BeginAnimation(OpacityProperty, tw);
            }
        }

        // one gentle drift for the whole field: a single GPU-composited transform,
        // slow auto-reverse so it wanders without ever showing an edge. Near-zero CPU.
        var drift = new TranslateTransform();
        StarsCanvas.RenderTransform = drift;
        drift.BeginAnimation(TranslateTransform.XProperty, new DoubleAnimation(0, -22, TimeSpan.FromSeconds(48))
        { AutoReverse = true, RepeatBehavior = RepeatBehavior.Forever });
        drift.BeginAnimation(TranslateTransform.YProperty, new DoubleAnimation(0, -14, TimeSpan.FromSeconds(67))
        { AutoReverse = true, RepeatBehavior = RepeatBehavior.Forever });
    }

    void Window_Drag(object sender, MouseButtonEventArgs e) { if (e.ButtonState == MouseButtonState.Pressed) DragMove(); }
    void Minimize_Click(object sender, MouseButtonEventArgs e) { e.Handled = true; WindowState = WindowState.Minimized; }
    void Close_Click(object sender, MouseButtonEventArgs e) { e.Handled = true; Close(); }
}
