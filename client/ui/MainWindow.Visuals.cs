using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;
using System.Windows.Threading;
using Brush = System.Windows.Media.Brush;
using Color = System.Windows.Media.Color;

namespace OrbitLan;

public partial class MainWindow
{
    sealed class EarthFrame
    {
        public required BitmapSource Image { get; init; }
        public int Left { get; init; }
        public int Top { get; init; }
        public int DelayMs { get; init; }
    }

    sealed class MeshNode
    {
        public required Ellipse Back { get; init; }
        public required Ellipse Front { get; init; }
        public double Longitude { get; init; }
        public double Latitude { get; init; }
        public double OrbitRadius { get; init; }
        public double X { get; set; }
        public double Y { get; set; }
        public double Depth { get; set; }
    }

    sealed class MeshLink
    {
        public required Line Back { get; init; }
        public required Line Front { get; init; }
        public int A { get; init; }
        public int B { get; init; }
    }

    readonly List<EarthFrame> earthFrames = new();
    readonly List<BitmapSource> headerFrames = new();
    readonly List<MeshNode> meshNodes = new();
    readonly List<MeshLink> meshLinks = new();
    readonly DispatcherTimer earthTimer = new();
    readonly DispatcherTimer visualTimer = new() { Interval = TimeSpan.FromMilliseconds(80) };
    readonly Stopwatch visualClock = new();
    readonly byte[] earthClearPixels = new byte[286 * 286 * 4];
    readonly byte[] earthFramePixels = new byte[286 * 286 * 4];
    WriteableBitmap? earthSurface;
    int earthFrameIndex;
    bool networkVisualLive;
    bool detailedVisualsCreated;
    bool animationsSuspended;
    int activeNetworkNodes;

    void StartVisuals()
    {
        LoadEarthFrames();
        ApplyPerformanceMode();
        Activated += (_, _) => ResumeDecorativeAnimations();
        Deactivated += (_, _) => SuspendDecorativeAnimations();
    }

    void SuspendDecorativeAnimations()
    {
        animationsSuspended = true;
        earthTimer.Stop();
        visualTimer.Stop();
    }

    void ResumeDecorativeAnimations()
    {
        animationsSuspended = false;
        if (earthFrames.Count > 0) earthTimer.Start();
        if (!_performanceMode && networkVisualLive)
        {
            visualClock.Start();
            visualTimer.Start();
        }
    }

    void EnsureDetailedVisuals()
    {
        PlanetGlow.Visibility = Visibility.Visible;
        NetworkBackCanvas.Visibility = Visibility.Visible;
        NetworkFrontCanvas.Visibility = Visibility.Visible;
        PlanetSway.BeginAnimation(System.Windows.Media.TranslateTransform.YProperty,
            new DoubleAnimation(5, -7, TimeSpan.FromSeconds(6.8))
            { AutoReverse = true, RepeatBehavior = RepeatBehavior.Forever });

        if (!detailedVisualsCreated)
        {
            CreateNetworkMesh();
            visualTimer.Tick += (_, _) => AdvanceNetworkMesh(visualClock.ElapsedMilliseconds);
            detailedVisualsCreated = true;
        }
    }

    void ApplyPerformanceMode()
    {
        if (!_performanceMode)
        {
            EnsureDetailedVisuals();
            SetNetworkVisualState(StatusText.Text switch
            {
                "Connecting" => "connecting",
                "Connected" => "connected",
                _ => "offline",
            });
            return;
        }

        StopPulse();
        activeNetworkNodes = 0;
        if (detailedVisualsCreated) AdvanceNetworkMesh(visualClock.ElapsedMilliseconds);
        visualTimer.Stop();
        visualClock.Stop();
        PlanetSway.BeginAnimation(System.Windows.Media.TranslateTransform.YProperty, null);
        PlanetSway.Y = 0;
        PlanetGlow.Visibility = Visibility.Collapsed;
        NetworkBackCanvas.Visibility = Visibility.Collapsed;
        NetworkFrontCanvas.Visibility = Visibility.Collapsed;
        AnimatePlanetScale(1, 0.35);
    }

    void LoadEarthFrames()
    {
        try
        {
            // WPF exposes later GIF frames as cropped rectangles. Since this generated GIF uses
            // disposal-to-background, each tick can clear one small surface and place the frame at
            // its metadata offset. This keeps the exact GIF without a large decoded frame atlas.
            var decoder = new GifBitmapDecoder(
                new Uri("pack://application:,,,/assets/earth-clouds.gif", UriKind.Absolute),
                BitmapCreateOptions.PreservePixelFormat,
                BitmapCacheOption.OnLoad);
            foreach (BitmapFrame decoded in decoder.Frames)
            {
                var frame = new FormatConvertedBitmap(decoded, PixelFormats.Pbgra32, null, 0);
                frame.Freeze();
                var metadata = decoded.Metadata as BitmapMetadata;
                int left = ReadGifMetadata(metadata, "/imgdesc/Left", 0);
                int top = ReadGifMetadata(metadata, "/imgdesc/Top", 0);
                int delay = Math.Max(20, ReadGifMetadata(metadata, "/grctlext/Delay", 10) * 10);
                earthFrames.Add(new EarthFrame
                {
                    Image = frame,
                    Left = left,
                    Top = top,
                    DelayMs = delay,
                });
            }

            var headerSheet = new BitmapImage();
            headerSheet.BeginInit();
            headerSheet.CacheOption = BitmapCacheOption.OnLoad;
            headerSheet.UriSource = new Uri("pack://application:,,,/assets/earth-header-sheet.png", UriKind.Absolute);
            headerSheet.EndInit();
            headerSheet.Freeze();
            for (int index = 0; index < earthFrames.Count; index++)
            {
                var headerFrame = new CroppedBitmap(headerSheet, new Int32Rect(
                    (index % 10) * 48,
                    (index / 10) * 48,
                    48,
                    48));
                headerFrame.Freeze();
                headerFrames.Add(headerFrame);
            }

            earthSurface = new WriteableBitmap(286, 286, 96, 96, PixelFormats.Pbgra32, null);
            EarthImage.Source = earthSurface;
            HeaderEarthImage.Source = headerFrames[0];
            RenderEarthFrame(earthFrames[0]);
            earthTimer.Interval = TimeSpan.FromMilliseconds(earthFrames[0].DelayMs);
            earthTimer.Tick += (_, _) =>
            {
                earthFrameIndex = (earthFrameIndex + 1) % earthFrames.Count;
                var next = earthFrames[earthFrameIndex];
                RenderEarthFrame(next);
                HeaderEarthImage.Source = headerFrames[earthFrameIndex];
                earthTimer.Interval = TimeSpan.FromMilliseconds(next.DelayMs);
            };
            earthTimer.Start();
        }
        catch
        {
            // Decorative artwork must never prevent connection.
        }
    }

    static int ReadGifMetadata(BitmapMetadata? metadata, string query, int fallback)
    {
        try { return Convert.ToInt32(metadata?.GetQuery(query) ?? fallback); }
        catch { return fallback; }
    }

    void RenderEarthFrame(EarthFrame frame)
    {
        if (earthSurface == null) return;
        const int surfaceStride = 286 * 4;
        earthSurface.WritePixels(new Int32Rect(0, 0, 286, 286), earthClearPixels, surfaceStride, 0);
        int frameStride = frame.Image.PixelWidth * 4;
        frame.Image.CopyPixels(earthFramePixels, frameStride, 0);
        earthSurface.WritePixels(
            new Int32Rect(frame.Left, frame.Top, frame.Image.PixelWidth, frame.Image.PixelHeight),
            earthFramePixels,
            frameStride,
            0);
    }

    void CreateNetworkMesh()
    {
        var lineBrush = new SolidColorBrush(Color.FromRgb(69, 242, 154));
        lineBrush.Freeze();
        var nodeBrush = new SolidColorBrush(Color.FromRgb(76, 255, 159));
        nodeBrush.Freeze();
        var nodeStroke = new SolidColorBrush(Color.FromRgb(7, 92, 60));
        nodeStroke.Freeze();

        (int A, int B)[] pairs =
        {
            (0, 1), (1, 2), (2, 0), (2, 3), (3, 0), (3, 4), (4, 1),
            (4, 5), (5, 2), (5, 6), (6, 3), (6, 7), (7, 4), (7, 0),
        };
        foreach (var (a, b) in pairs)
        {
            var back = MeshLine(lineBrush);
            var front = MeshLine(lineBrush);
            NetworkBackCanvas.Children.Add(back);
            NetworkFrontCanvas.Children.Add(front);
            meshLinks.Add(new MeshLink { Back = back, Front = front, A = a, B = b });
        }

        double[] latitudes = { -0.64, 0.08, 0.62, -0.22, 0.38, -0.48, 0.72, 0.18 };
        for (int i = 0; i < latitudes.Length; i++)
        {
            var back = MeshDot(nodeBrush, nodeStroke);
            var front = MeshDot(nodeBrush, nodeStroke);
            NetworkBackCanvas.Children.Add(back);
            NetworkFrontCanvas.Children.Add(front);
            meshNodes.Add(new MeshNode
            {
                Back = back,
                Front = front,
                Longitude = i * (Math.PI * 2 / latitudes.Length) + (i % 2) * 0.34,
                Latitude = latitudes[i],
                OrbitRadius = 122 + (i % 3) * 4,
            });
        }
        AdvanceNetworkMesh(0);
    }

    static Line MeshLine(Brush brush) => new()
    {
        Stroke = brush,
        StrokeThickness = 1.7,
        StrokeDashArray = new DoubleCollection { 2.2, 2.8 },
        IsHitTestVisible = false,
        Opacity = 0,
    };

    static Ellipse MeshDot(Brush fill, Brush stroke) => new()
    {
        Fill = fill,
        Stroke = stroke,
        StrokeThickness = 1.6,
        IsHitTestVisible = false,
        Opacity = 0,
    };

    void AdvanceNetworkMesh(long elapsedMs)
    {
        double time = elapsedMs / 1000d;
        const double center = 143;
        for (int i = 0; i < meshNodes.Count; i++)
        {
            var node = meshNodes[i];
            if (i >= activeNetworkNodes)
            {
                node.Back.Opacity = node.Front.Opacity = 0;
                continue;
            }

            double longitude = node.Longitude + time * (0.19 + i % 3 * 0.012);
            double latitude = node.Latitude + Math.Sin(time * 0.31 + i * 0.8) * 0.06;
            double cosLatitude = Math.Cos(latitude);
            node.Depth = Math.Cos(longitude) * cosLatitude;
            node.X = center + Math.Sin(longitude) * cosLatitude * node.OrbitRadius;
            node.Y = center - Math.Sin(latitude) * 103 + node.Depth * 10;

            double size = 8.5 + (node.Depth + 1) * 2.4;
            PositionMeshDot(node.Back, node.X, node.Y, size);
            PositionMeshDot(node.Front, node.X, node.Y, size);
            bool front = node.Depth >= 0;
            node.Back.Opacity = front ? 0 : 0.32 + (node.Depth + 1) * 0.12;
            node.Front.Opacity = front ? 0.78 + node.Depth * 0.2 : 0;
        }

        for (int i = 0; i < meshLinks.Count; i++)
        {
            var link = meshLinks[i];
            if (link.A >= activeNetworkNodes || link.B >= activeNetworkNodes)
            {
                link.Back.Opacity = link.Front.Opacity = 0;
                continue;
            }

            var a = meshNodes[link.A];
            var b = meshNodes[link.B];
            SetMeshLine(link.Back, a, b);
            SetMeshLine(link.Front, a, b);
            double depth = (a.Depth + b.Depth) / 2;
            bool front = depth >= 0;
            link.Back.Opacity = front ? 0 : 0.18 + (depth + 1) * 0.12;
            link.Front.Opacity = front ? 0.48 + depth * 0.3 : 0;
            link.Back.StrokeDashOffset = link.Front.StrokeDashOffset = -(time * 7 + i * 0.7);
        }
    }

    static void PositionMeshDot(Ellipse dot, double x, double y, double size)
    {
        dot.Width = dot.Height = size;
        Canvas.SetLeft(dot, x - size / 2);
        Canvas.SetTop(dot, y - size / 2);
    }

    static void SetMeshLine(Line line, MeshNode a, MeshNode b)
    {
        line.X1 = a.X;
        line.Y1 = a.Y;
        line.X2 = b.X;
        line.Y2 = b.Y;
    }

    void SetNetworkVisualState(string state)
    {
        networkVisualLive = state == "connected";
        if (_performanceMode)
        {
            StopPulse();
            activeNetworkNodes = 0;
            AnimatePlanetScale(1, 0.35);
            return;
        }
        if (state == "offline")
        {
            StopPulse();
            StopMeshAnimation();
            UpdateNetworkNodes(-1);
            AnimatePlanetScale(1, 0.5);
            return;
        }

        if (state == "connecting")
        {
            StopMeshAnimation();
            AnimatePlanetScale(1, 0.4);
            StartPulse(1.2, Color.FromRgb(126, 145, 255));
            UpdateNetworkNodes(-1);
            return;
        }

        // The sonar communicates connection progress only. Once live, the expanding globe and
        // orbiting mesh take over without changing the layout measurements around them.
        StopPulse();
        visualClock.Start();
        if (!animationsSuspended) visualTimer.Start();
        AnimatePlanetScale(1.14, 0.62);
        UpdateNetworkNodes(rows.Count(row => row.StatusLabel is "Direct" or "Relay"));
    }

    void StopMeshAnimation()
    {
        visualTimer.Stop();
        visualClock.Stop();
    }

    void AnimatePlanetScale(double target, double seconds)
    {
        double current = PlanetVisualScale.ScaleX;
        var easing = new CubicEase { EasingMode = EasingMode.EaseOut };
        PlanetVisualScale.BeginAnimation(ScaleTransform.ScaleXProperty,
            new DoubleAnimation(current, target, TimeSpan.FromSeconds(seconds)) { EasingFunction = easing });
        PlanetVisualScale.BeginAnimation(ScaleTransform.ScaleYProperty,
            new DoubleAnimation(current, target, TimeSpan.FromSeconds(seconds)) { EasingFunction = easing });
    }

    void StartPulse(double seconds, Color color)
    {
        ConnectionPulse.Stroke = new SolidColorBrush(color);
        ConnectionPulse.BeginAnimation(OpacityProperty,
            new DoubleAnimation(0.82, 0, TimeSpan.FromSeconds(seconds))
            { RepeatBehavior = RepeatBehavior.Forever });
        var scale = new DoubleAnimation(0.84, 1.55, TimeSpan.FromSeconds(seconds))
        {
            RepeatBehavior = RepeatBehavior.Forever,
            EasingFunction = new QuadraticEase { EasingMode = EasingMode.EaseOut },
        };
        ConnectionPulseScale.BeginAnimation(System.Windows.Media.ScaleTransform.ScaleXProperty, scale);
        ConnectionPulseScale.BeginAnimation(System.Windows.Media.ScaleTransform.ScaleYProperty, scale);
    }

    void StopPulse()
    {
        ConnectionPulse.BeginAnimation(OpacityProperty, null);
        ConnectionPulse.Opacity = 0;
        ConnectionPulseScale.BeginAnimation(System.Windows.Media.ScaleTransform.ScaleXProperty, null);
        ConnectionPulseScale.BeginAnimation(System.Windows.Media.ScaleTransform.ScaleYProperty, null);
        ConnectionPulseScale.ScaleX = ConnectionPulseScale.ScaleY = 0.84;
    }

    void UpdateNetworkNodes(int peerCount)
    {
        // The local client and room endpoint establish the first edge; connected peers grow the
        // surrounding mesh. Back-half elements sit behind the Earth and front-half elements above.
        activeNetworkNodes = networkVisualLive ? Math.Clamp(peerCount + 2, 2, meshNodes.Count) : 0;
        AdvanceNetworkMesh(visualClock.ElapsedMilliseconds);
    }

    protected override void OnClosed(EventArgs e)
    {
        earthTimer.Stop();
        visualTimer.Stop();
        DisposeTray();
        engine.Stop();
        base.OnClosed(e);
    }
}
