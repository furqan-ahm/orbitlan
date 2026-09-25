using System.Windows;

namespace OrbitLan;

public partial class App : Application
{
    protected override void OnExit(ExitEventArgs e)
    {
        Engine.Instance.Stop();
        base.OnExit(e);
    }
}
