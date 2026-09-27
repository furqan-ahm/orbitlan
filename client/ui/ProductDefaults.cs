namespace OrbitLan;

/// <summary>
/// Public service defaults for official builds. Users can override the coordinator in Settings.
/// The coordinator can be changed by users who prefer to self-host.
/// </summary>
public static class ProductDefaults
{
    public const string CoordinatorUrl = "https://orbitlan.furqan-ahm.workers.dev";
    public const string UpdateManifestUrl =
        "https://github.com/furqan-ahm/orbitlan/releases/latest/download/manifest.json";

    public const string SupportUrl =
        "https://www.patreon.com/orbitlan/posts/orbitlan-edition-170621986";
}
