using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;

namespace LanPlayerSetup;

public sealed class MainWindow : Window
{
    private readonly GameInstall _install;
    private readonly TextBox _name = new() { MaxLength = MiiDatabase.NameLength, Width = 220, HorizontalAlignment = HorizontalAlignment.Left };
    private readonly RadioButton _boy = new() { Content = "Male", GroupName = "gender", IsChecked = true };
    private readonly RadioButton _girl = new() { Content = "Female", GroupName = "gender" };
    private readonly ComboBox _color = new() { ItemsSource = MiiDatabase.FavoriteColors, Width = 220, SelectedIndex = 0 };
    private readonly TextBlock _status = new() { TextWrapping = TextWrapping.Wrap };
    private readonly Button _save = new() { Content = "Save" };
    private readonly Button _play = new() { Content = "Save and play", IsDefault = true };

    public MainWindow(GameInstall install)
    {
        _install = install;
        Title = "Mario Kart Wii LAN - player";
        Width = 460;
        SizeToContent = SizeToContent.Height;
        CanResize = false;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;

        var current = LoadCurrent(out var loadError);
        _name.Text = current?.Name ?? MiiDatabase.DefaultName;
        if (current is not null)
        {
            _girl.IsChecked = current.IsGirl;
            _boy.IsChecked = !current.IsGirl;
            _color.SelectedIndex = current.FavoriteColor;
        }

        _save.Click += (_, _) => Apply(startGame: false);
        _play.Click += (_, _) => Apply(startGame: true);
        _play.IsEnabled = install.GameExecutable is not null;

        var hint = new TextBlock
        {
            TextWrapping = TextWrapping.Wrap,
            Opacity = 0.8,
            Text = current is null
                ? "This creates a Mii with your name. The first time, select it for your license in the " +
                  "game: in the license's settings choose Change Mii, or create a new license with it. " +
                  "After that, changing the name here is enough."
                : "Your license keeps using this Mii, so the new name shows up the next time you play.",
        };

        _status.Text = loadError ?? (install.GameExecutable is null
            ? "The game was not found next to this program, so it cannot be started from here."
            : null);

        Content = new StackPanel
        {
            Margin = new Thickness(20),
            Spacing = 10,
            Children =
            {
                new TextBlock { Text = "Player name (shown to other players)", FontWeight = FontWeight.SemiBold },
                _name,
                new StackPanel { Orientation = Orientation.Horizontal, Spacing = 16, Children = { _boy, _girl } },
                new TextBlock { Text = "Favorite color" },
                _color,
                hint,
                _status,
                new StackPanel
                {
                    Orientation = Orientation.Horizontal,
                    HorizontalAlignment = HorizontalAlignment.Right,
                    Spacing = 8,
                    Children = { _save, _play },
                },
                new TextBlock { Text = $"Mii database: {install.MiiDatabasePath}", FontSize = 11, Opacity = 0.6, TextWrapping = TextWrapping.Wrap },
            },
        };
    }

    private PlayerMii? LoadCurrent(out string? error)
    {
        error = null;
        try
        {
            return MiiDatabase.Load(_install.MiiDatabasePath).FindOwn();
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            error = ex.Message;
            return null;
        }
    }

    private void Apply(bool startGame)
    {
        try
        {
            var mii = new PlayerMii(_name.Text?.Trim() ?? "", _girl.IsChecked == true, Math.Max(0, _color.SelectedIndex));
            PlayerSetup.Save(_install, mii);
            if (startGame)
            {
                _install.StartGame();
                Close();
                return;
            }
            _status.Text = $"Saved. Your name is now \"{mii.Name}\".";
        }
        catch (Exception ex) when (ex is ArgumentException or IOException or UnauthorizedAccessException
                                       or InvalidDataException or InvalidOperationException or System.ComponentModel.Win32Exception)
        {
            _status.Text = ex.Message;
        }
    }
}
