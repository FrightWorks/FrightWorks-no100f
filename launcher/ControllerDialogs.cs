using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Platform.Storage;

namespace FrightWorks;

// Controller-originated flows use local themed browsers. Mouse/keyboard retain the native storage provider/portal.
static class ControllerDialogs
{
    // A borderless popup: no operating-system title bar or frame, so it reads as the launcher's own dropdown. The themed
    // edge keeps it visible on top of the launcher, and the setting's name is its heading. Still a modal owned window
    // (centred on the launcher; Escape cancels, a click selects, a double click applies).
    internal static async Task<int?> Choose(Window owner, object?[] items, int selected, string title = "Choose a setting")
    {
        var dialog = Dialogs.Make(title, owner);
        dialog.WindowDecorations = WindowDecorations.None;
        // The list is as tall as its rows (they are not a fixed 30 px), up to a cap that leaves room for the window around it;
        // longer lists scroll.
        double cap = Math.Min(480, Math.Max(200, owner.Bounds.Height - 220));
        var list = new ControllerList { ItemsSource = items, SelectedIndex = selected, MaxHeight = cap, MinWidth = 300,
                                       Background = Skin.Brush(Skin.Field), Foreground = Skin.Brush(Skin.Cream), BorderBrush = Skin.Brush(Skin.Purple) };
        int? answer = null;
        void Apply() { if (list.SelectedIndex >= 0) { answer = list.SelectedIndex; dialog.Close(); } }
        list.Activate = Apply;
        list.DoubleTapped += (_, _) => Apply();
        var heading = Skin.Title(title);
        heading.Margin = new Thickness(0, 0, 0, 8);
        var body = Body(list, new PlankButton("Apply", (_, _) => Apply(), true),
                        new PlankButton("Cancel", (_, _) => dialog.Close()));
        body.Children.Insert(0, heading);
        dialog.Content = new Border { BorderBrush = Skin.Brush(Skin.Purple), BorderThickness = new Thickness(3), Child = body };
        dialog.Opened += (_, _) => { list.Focus(NavigationMethod.Directional); list.ScrollIntoView(list.SelectedIndex); };
        await Dialogs.ShowModal(dialog, owner);
        return answer;
    }

    internal static async Task<string?> Text(Window owner, string title, string initial)
    {
        var dialog = Dialogs.Make(title, owner);
        var field = new ControllerTextBox { Text = initial, MinWidth = 480, Margin = new Thickness(0, 0, 0, 8) };
        var body = new StackPanel { Margin = new Thickness(18) };
        body.Children.Add(field);
        var keys = new Grid { ColumnDefinitions = new ColumnDefinitions("*,*,*,*,*,*,*,*,*,*") };
        string[] letters = { "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm" };
        var buttons = new List<(PlankButton Button, char Letter)>();
        bool upper = false, symbols = false;
        const string punctuation = "!@#$%^&*()-_=+[]{};:'\",.<>/?\\|`~";
        int keyIndex = 0;
        foreach (string row in letters)
        {
            keys.RowDefinitions.Add(new RowDefinition(GridLength.Auto));
            foreach (char letter in row)
            {
                var button = new PlankButton(letter.ToString(), (sender, _) => field.SelectedText = ((PlankButton)sender!).Text)
                { Margin = new Thickness(1), HorizontalAlignment = HorizontalAlignment.Stretch };
                Grid.SetRow(button, keyIndex / 10);
                Grid.SetColumn(button, keyIndex % 10);
                buttons.Add((button, letter));
                keys.Children.Add(button);
                keyIndex++;
            }
            keyIndex = ((keyIndex + 9) / 10) * 10;
        }
        body.Children.Add(keys);
        var editing = new WrapPanel();
        editing.Children.Add(new PlankButton("Space", (_, _) => field.SelectedText = " "));
        editing.Children.Add(new PlankButton("Backspace", (_, _) =>
        {
            var value = field.Text ?? "";
            int start = Math.Min(field.SelectionStart, field.SelectionEnd), end = Math.Max(field.SelectionStart, field.SelectionEnd);
            if (start == end && start > 0) start = StringInfo.ParseCombiningCharacters(value[..start])[^1];
            field.Text = value.Remove(start, end - start);
            field.SelectionStart = field.SelectionEnd = start;
        }));
        editing.Children.Add(new PlankButton("Shift", (_, _) => { upper = !upper; Labels(); }));
        editing.Children.Add(new PlankButton("Symbols", (_, _) => { symbols = !symbols; Labels(); }));
        editing.Children.Add(new PlankButton("Clear", (_, _) => field.Text = ""));
        body.Children.Add(editing);
        string? answer = null;
        var actions = new WrapPanel();
        actions.Children.Add(new PlankButton("Apply", (_, _) => { answer = field.Text ?? ""; dialog.Close(); }, true));
        actions.Children.Add(new PlankButton("Cancel", (_, _) => dialog.Close()));
        body.Children.Add(actions);
        dialog.Content = body;
        dialog.Opened += (_, _) =>
        {
            field.CaretIndex = (field.Text ?? "").Length;
            buttons[0].Button.Focus(NavigationMethod.Directional);
        };
        await Dialogs.ShowModal(dialog, owner);
        return answer;

        void Labels()
        {
            for (int i = 0; i < buttons.Count; i++)
                buttons[i].Button.Text = symbols ? punctuation[i % punctuation.Length].ToString() :
                    (upper ? char.ToUpperInvariant(buttons[i].Letter) : buttons[i].Letter).ToString();
        }
    }

    static StackPanel Body(Control content, params PlankButton[] buttons)
    {
        var body = new StackPanel { Margin = new Thickness(18) };
        body.Children.Add(content);
        var row = new WrapPanel { Margin = new Thickness(0, 8, 0, 0) };
        foreach (var button in buttons) row.Children.Add(button);
        body.Children.Add(row);
        return body;
    }

    // The themed browser when the pad started this action (decided at once, before any await), else the native picker or
    // portal. `viaController` is for tests; callers leave it out.
    public static async Task<string?> PickPath(Window owner, string title, string? start = null, bool folder = false, bool? viaController = null,
                                                  string[]? extensions = null, string? typeName = null)
    {
        // Disc images unless the caller names its own file type (e.g. ".zip"); extensions are lower case with the dot.
        extensions ??= DiscExtensions;
        typeName ??= "GameCube disc images (*.iso, *.gcm, *.ciso)";
        if (viaController ?? ControllerNavigation.For(owner)?.Activating == true) return await Browse(owner, title, start, folder, extensions);
        if (folder)
        {
            var picked = await owner.StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions
            {
                Title = title, AllowMultiple = false,
                SuggestedStartLocation = start != null && Directory.Exists(start) ? await owner.StorageProvider.TryGetFolderFromPathAsync(start) : null,
            });
            return picked.Count == 0 ? null : Local(picked[0]);
        }
        var files = await owner.StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = title, AllowMultiple = false, FileTypeFilter = new[]
            {
                new FilePickerFileType(typeName) { Patterns = extensions.SelectMany(e => new[] { "*" + e, "*" + e.ToUpperInvariant() }).ToArray() },
                FilePickerFileTypes.All,
            },
        });
        return files.Count == 0 ? null : Local(files[0]);

        static string Local(IStorageItem item) => item.TryGetLocalPath() ??
            throw new IOException("The selected item has no local filesystem path. Choose a local folder or file.");
    }

    static readonly string[] DiscExtensions = { ".iso", ".gcm", ".ciso" };

    sealed record Entry(string Path, bool Directory)
    {
        public override string ToString() => (Directory ? "[Folder] " : "") + System.IO.Path.GetFileName(Path);
    }

    internal static async Task<string?> Browse(Window owner, string title, string? start, bool folder, string[]? extensions = null)
    {
        extensions ??= DiscExtensions;
        var dialog = Dialogs.Make(title, owner);
        dialog.SizeToContent = SizeToContent.Manual;
        dialog.Width = Math.Min(720, owner.Bounds.Width - 30);
        dialog.Height = Math.Min(560, owner.Bounds.Height - 30);
        var current = Path.GetFullPath(start ?? Environment.GetFolderPath(Environment.SpecialFolder.UserProfile));
        while (!Directory.Exists(current) && Directory.GetParent(current) is { } parent) current = parent.FullName;
        var pathLabel = Skin.Note(current);
        var list = new ControllerList { MinHeight = 120, Background = Skin.Brush(Skin.Field),
                                       Foreground = Skin.Brush(Skin.Cream), BorderBrush = Skin.Brush(Skin.Purple) };
        bool all = folder;
        string? answer = null;
        var grid = new Grid { Margin = new Thickness(18), RowDefinitions = new RowDefinitions("Auto,Auto,*,Auto") };
        grid.Children.Add(pathLabel);
        var tools = new WrapPanel();
        tools.Children.Add(new PlankButton("Up a folder", (_, _) =>
        {
            if (Directory.GetParent(current) is { } parent) Load(parent.FullName).Watch();
        }));
        tools.Children.Add(new PlankButton("Locations", async (_, _) =>
        {
            var roots = DriveInfo.GetDrives().Select(d => d.RootDirectory.FullName).ToArray();
            int? choice = await Choose(dialog, roots.Cast<object?>().ToArray(), 0, "Locations");
            if (choice is int index) await Load(roots[index]);
        }));
        tools.Children.Add(new PlankButton("Enter path", async (_, _) =>
        {
            var path = await Text(dialog, "Enter a folder path", current);
            if (path != null) await Load(path);
        }));
        if (folder) tools.Children.Add(new PlankButton("New folder", async (_, _) =>
        {
            var name = await Text(dialog, "New folder name", "");
            if (name == null) return;
            if (string.IsNullOrWhiteSpace(name) || name is "." or ".." || name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 ||
                name.Contains(Path.DirectorySeparatorChar) || name.Contains(Path.AltDirectorySeparatorChar))
            { await Dialogs.Message(dialog, "Enter a single valid folder name."); return; }
            try { Directory.CreateDirectory(Path.Combine(current, name)); await Load(current); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException)
            { await Dialogs.Message(dialog, "Couldn't create the folder: " + e.Message); }
        }));
        if (!folder)
        {
            var showAll = new CheckBox { Content = "Show all files", Margin = new Thickness(8) };
            showAll.IsCheckedChanged += (_, _) => { all = showAll.IsChecked == true; Load(current).Watch(); };
            tools.Children.Add(showAll);
        }
        Grid.SetRow(tools, 1);
        grid.Children.Add(tools);
        Grid.SetRow(list, 2);
        grid.Children.Add(list);
        var actions = new WrapPanel();
        actions.Children.Add(new PlankButton(folder ? "Use this folder" : "Open", (_, _) => Accept(false).Watch(), true));
        actions.Children.Add(new PlankButton("Cancel", (_, _) => dialog.Close()));
        Grid.SetRow(actions, 3);
        grid.Children.Add(actions);
        list.Activate = () => Accept(true).Watch();
        dialog.Content = grid;
        dialog.Opened += (_, _) => list.Focus(NavigationMethod.Directional);
        await Load(current);
        await Dialogs.ShowModal(dialog, owner);
        return answer;

        async Task Load(string path)
        {
            try
            {
                var full = Path.GetFullPath(path);
                var entries = Directory.EnumerateDirectories(full).OrderBy(p => p, StringComparer.OrdinalIgnoreCase)
                    .Select(p => new Entry(p, true)).ToList();
                if (!folder) entries.AddRange(Directory.EnumerateFiles(full)
                    .Where(p => all || extensions.Contains(Path.GetExtension(p).ToLowerInvariant()))
                    .OrderBy(p => p, StringComparer.OrdinalIgnoreCase).Select(p => new Entry(p, false)));
                current = full;
                pathLabel.Text = current;
                list.ItemsSource = entries;
                list.SelectedIndex = entries.Count > 0 ? 0 : -1;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
            { await Dialogs.Message(dialog.IsVisible ? dialog : owner, "Couldn't read the folder: " + e.Message); }
        }

        async Task Accept(bool browseSelected)
        {
            if (folder && browseSelected && list.SelectedItem is Entry { Directory: true } directory)
            { await Load(directory.Path); return; }
            if (folder)
            {
                if (!Directory.Exists(current)) { await Dialogs.Message(dialog, "The folder no longer exists."); return; }
                answer = current;
                dialog.Close();
            }
            else if (list.SelectedItem is Entry entry)
            {
                if (entry.Directory) { await Load(entry.Path); return; }
                if (!File.Exists(entry.Path)) { await Dialogs.Message(dialog, "The file no longer exists."); return; }
                answer = entry.Path;
                dialog.Close();
            }
        }
    }
}

sealed class ControllerTextBox : TextBox
{
    protected override Type StyleKeyOverride => typeof(TextBox);
}
