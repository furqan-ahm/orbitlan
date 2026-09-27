using System.Windows;
using Application = System.Windows.Application;

namespace OrbitLan;

public partial class App : Application
{
    protected override void OnExit(ExitEventArgs e)
    {
        Engine.Instance.Stop();
        base.OnExit(e);
    }
}
