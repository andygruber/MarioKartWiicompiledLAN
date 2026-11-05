using System.Diagnostics;
using WiiCompiled.Setup.Common;

namespace LanPlayerSetup;

/// <summary>
/// Where the game and its user data live, resolved the same way the runtime does
/// (<c>runtime/include/runtime_config.h</c> and <c>nand_path.h</c>): a portable root next to the
/// game wins, otherwise the per-user application data directory (%LOCALAPPDATA% on Windows,
/// $XDG_DATA_HOME or ~/.local/share on Linux); <c>[paths] nand_root</c> overrides the NAND.
/// </summary>
public sealed class GameInstall
{
    public const string ApplicationDirectoryName = "MarioKartWiicompiledLAN";
    private static readonly string[] GameExecutableNames = OperatingSystem.IsWindows()
        ? ["WiiCompiled.exe", "MarioKartWiicompiledLAN.exe"]
        : ["WiiCompiled", "MarioKartWiicompiledLAN"];

    private GameInstall(string? gameExecutable, string userDirectory)
    {
        GameExecutable = gameExecutable;
        UserDirectory = userDirectory;
        ConfigPath = Path.Combine(userDirectory, RuntimeConfiguration.ConfigFileName);
        NandDirectory = RuntimeConfiguration.GetResolvedPath(ConfigPath, "nand_root") ?? Path.Combine(userDirectory, "NAND");
        MiiDatabasePath = Path.Combine(NandDirectory, MiiDatabase.RelativePath);
    }

    public string? GameExecutable { get; }
    public string UserDirectory { get; }
    public string ConfigPath { get; }
    public string NandDirectory { get; }
    public string MiiDatabasePath { get; }

    /// <param name="gameExecutable">The game, or null to look next to this program.</param>
    /// <param name="userDirectory">The directory holding Config.toml, or null for the game's default.</param>
    public static GameInstall Resolve(string? gameExecutable, string? userDirectory)
    {
        gameExecutable = gameExecutable is null ? FindGameNextToUs() : Path.GetFullPath(gameExecutable);
        if (userDirectory is not null) return new GameInstall(gameExecutable, Path.GetFullPath(userDirectory));

        var gameDirectory = gameExecutable is null ? AppContext.BaseDirectory : Path.GetDirectoryName(gameExecutable)!;
        if (PortableRoot.TryFind(gameDirectory) is { } portableRoot)
            return new GameInstall(gameExecutable, PortableRoot.UserDataDirectory(portableRoot));
        var appData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData, Environment.SpecialFolderOption.DoNotVerify);
        return new GameInstall(gameExecutable, Path.Combine(appData, ApplicationDirectoryName));
    }

    public Process StartGame()
    {
        if (GameExecutable is null || !File.Exists(GameExecutable))
            throw new FileNotFoundException("The game was not found next to this program. Pass --game <path>.");
        var start = new ProcessStartInfo(GameExecutable)
        {
            WorkingDirectory = Path.GetDirectoryName(GameExecutable)!,
            UseShellExecute = false,
        };
        return Process.Start(start) ?? throw new InvalidOperationException("The game could not be started.");
    }

    private static string? FindGameNextToUs()
    {
        foreach (var name in GameExecutableNames)
        {
            var candidate = Path.Combine(AppContext.BaseDirectory, name);
            if (File.Exists(candidate)) return candidate;
        }
        return null;
    }
}
