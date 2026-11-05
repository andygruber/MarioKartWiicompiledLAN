using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Themes.Fluent;

namespace LanPlayerSetup;

public static class PlayerSetup
{
    public static void Save(GameInstall install, PlayerMii mii)
    {
        var database = MiiDatabase.Load(install.MiiDatabasePath);
        database.SetOwn(mii);
        database.Save(install.MiiDatabasePath);
    }
}

public sealed class App : Application
{
    public static GameInstall Install { get; set; } = null!;

    public override void Initialize() => Styles.Add(new FluentTheme());

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
            desktop.MainWindow = new MainWindow(Install);
        base.OnFrameworkInitializationCompleted();
    }
}

public static class Program
{
    private const string Usage = """
        Sets the player name (a Mii) other players see in Mario Kart Wii LAN play, then starts the game.

          LanPlayerSetup [options]               opens the window
          LanPlayerSetup --no-gui [options]      applies the options without a window

        Options:
          --name NAME       player name, 1-10 characters (default: keep the current one, or "Player")
          --female, --male  the Mii's face (default: keep, or male)
          --color N         favorite color 0-11 (red, orange, yellow, light green, green, blue,
                            light blue, pink, purple, brown, white, black)
          --user-dir DIR    the game's user directory holding Config.toml
                            (default: the one the game uses)
          --game PATH       the game executable (default: next to this program)
          --start           with --no-gui: start the game afterwards
          --list            with --no-gui: list the Miis in the database
        """;

    [STAThread]
    public static int Main(string[] args)
    {
        string? name = null, userDir = null, game = null;
        bool? girl = null;
        int? color = null;
        bool noGui = false, start = false, list = false;
        try
        {
            for (var i = 0; i < args.Length; i++)
            {
                string Next() => i + 1 < args.Length ? args[++i] : throw new ArgumentException($"{args[i]} needs a value.");
                switch (args[i])
                {
                    case "--name": name = Next(); break;
                    case "--female": girl = true; break;
                    case "--male": girl = false; break;
                    case "--color": color = int.Parse(Next()); break;
                    case "--user-dir": userDir = Next(); break;
                    case "--game": game = Next(); break;
                    case "--no-gui": noGui = true; break;
                    case "--start": start = true; break;
                    case "--list": list = true; break;
                    case "-h" or "--help": Console.WriteLine(Usage); return 0;
                    default: throw new ArgumentException($"Unknown option {args[i]}.");
                }
            }

            var install = GameInstall.Resolve(game, userDir);
            if (!noGui)
            {
                App.Install = install;
                AppBuilder.Configure<App>().UsePlatformDetect().WithInterFont().LogToTrace().StartWithClassicDesktopLifetime(args);
                return 0;
            }

            var database = MiiDatabase.Load(install.MiiDatabasePath);
            if (list)
            {
                foreach (var (slot, miiName, creator) in database.Miis())
                    Console.WriteLine($"{slot,3}  {miiName}{(creator == MiiDatabase.OwnCreator ? "  (this program's Mii)" : "")}");
                return 0;
            }
            var current = database.FindOwn();
            var mii = new PlayerMii(
                name ?? current?.Name ?? MiiDatabase.DefaultName,
                girl ?? current?.IsGirl ?? false,
                color ?? current?.FavoriteColor ?? 0);
            PlayerSetup.Save(install, mii);
            Console.WriteLine($"Player name \"{mii.Name}\" saved to {install.MiiDatabasePath}");
            if (start) install.StartGame();
            return 0;
        }
        catch (Exception ex) when (ex is ArgumentException or FormatException or IOException or UnauthorizedAccessException
                                       or InvalidDataException or InvalidOperationException or System.ComponentModel.Win32Exception)
        {
            Console.Error.WriteLine($"error: {ex.Message}");
            return 1;
        }
    }
}
