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
        if (viaController ?? ControllerNavigation.For(owner)?.Activating == true) return await Browse(owner, title, start, folder, extensions, typeName);
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

    // The pad's file browser: a full-window menu like the launcher's others, in a borderless window over the launcher. A row
    // per folder and file: A opens a folder or chooses the file, X goes up a folder, B cancels. The drives and the usual
    // folders, a typed path and Show all files (or New folder) are rows at the end. Hidden and system folders are left out.
    internal static async Task<string?> Browse(Window owner, string title, string? start, bool folder, string[]? extensions = null, string? typeName = null)
    {
        extensions ??= DiscExtensions;
        typeName ??= "GameCube disc images (*.iso, *.gcm, *.ciso)";
        var dialog = Dialogs.Make(title, owner);
        dialog.WindowDecorations = WindowDecorations.None;
        dialog.SizeToContent = SizeToContent.Manual;
        dialog.Width = Math.Max(480, owner.Bounds.Width);
        dialog.Height = Math.Max(400, owner.Bounds.Height);
        if (owner.IsVisible)
        {
            dialog.WindowStartupLocation = WindowStartupLocation.Manual;
            dialog.Position = owner.PointToScreen(default);
        }
        string heading = folder ? "Choose a folder" : extensions.SequenceEqual(DiscExtensions) ? "Choose a disc" : "Choose a file";
        var screen = new MenuScreen(heading, () => dialog.Close()) { IsVisible = true, BackLabel = "Cancel" };
        var page = screen.Body;
        string instruction = title.TrimEnd('.') + ".";
        string? current = Path.GetFullPath(start ?? Environment.GetFolderPath(Environment.SpecialFolder.UserProfile));
        while (!Directory.Exists(current) && Directory.GetParent(current) is { } parent) current = parent.FullName;
        if (!Directory.Exists(current)) current = null;  // the drives and folders
        bool all = false;
        string? answer = null;
        dialog.Content = screen;
        if (!Show(current, null)) Show(null, null);
        dialog.Opened += (_, _) => Reselect(null);
        await Dialogs.ShowModal(dialog, owner);
        return answer;

        // A folder's rows, or (path null) the drives and the usual folders. False: the folder couldn't be read (said).
        bool Show(string? path, string? select)
        {
            List<string> dirs = new(), files = new();
            if (path != null)
            {
                try
                {
                    path = Path.GetFullPath(path);
                    dirs = Directory.EnumerateDirectories(path).Where(Shown).OrderBy(d => d, StringComparer.OrdinalIgnoreCase).ToList();
                    if (!folder)
                        files = Directory.EnumerateFiles(path).Where(f => all || extensions.Contains(Path.GetExtension(f).ToLowerInvariant()))
                            .OrderBy(f => f, StringComparer.OrdinalIgnoreCase).ToList();
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException or System.Security.SecurityException)
                {
                    Dialogs.Message(dialog.IsVisible ? dialog : owner, "Couldn't read the folder: " + e.Message).Watch();
                    return false;
                }
            }
            current = path;
            page.Children.Clear();
            if (path == null) Places();
            else
            {
                string name = Path.GetFileName(Path.TrimEndingDirectorySeparator(path)) is { Length: > 0 } n ? n : path;
                string hint = path;
                if (dirs.Count == 0 && files.Count == 0)
                    hint += folder ? "\nNo folders in here." : "\nNo folders or " + typeName + " in here." + (all ? "" : " Show all files (below) lists every file.");
                Heading(name, hint);
                if (folder)
                    page.Children.Add(new ActionRow("Use this folder", () => UseFolder().Watch(), "Choose", "Choose " + path + ".\n\n" + instruction));
                string up = Directory.GetParent(path) is { } parent ? "Back to " + parent.FullName + "." : "Back to the drives and your folders.";
                page.Children.Add(new ActionRow("Up a folder", Up, "Open", up + "\n\nX does this from any row.") { Value = ".." });
                foreach (var d in dirs)
                    page.Children.Add(new ActionRow(Path.GetFileName(d), () => Show(d, null), "Open", d) { Value = "Folder", Tag = d });
                foreach (var f in files)
                    page.Children.Add(new ActionRow(Path.GetFileName(f), () => Pick(f).Watch(), "Choose", f + "\n\n" + instruction)
                                      { Value = Size(f), ValueColor = Skin.Cream, Tag = f });
                Heading("Elsewhere");
                page.Children.Add(new ActionRow("Drives and folders", () => Show(null, path), "Open", "Your drives, and your home, Desktop, Documents and Downloads folders."));
                page.Children.Add(new ActionRow("Type a path", () => TypePath().Watch(), "Type", "Type the path of a folder" + (folder ? "" : " or a file") + "."));
                if (folder) page.Children.Add(new ActionRow("New folder", () => NewFolder().Watch(), "Create", "Make a folder in " + path + "."));
                else
                {
                    var showAll = new ToggleRow("Show all files", "Off: only " + typeName + " and folders. On: every file.") { IsChecked = all };
                    showAll.IsCheckedChanged += (_, _) => { all = showAll.IsChecked == true; Show(current, "Show all files"); };
                    page.Children.Add(showAll);
                }
            }
            screen.ScreenExtra = path == null ? null : ("Up a folder", Up);
            if (dialog.IsVisible) Reselect(select);
            return true;
        }

        void Places()
        {
            Heading("Your folders");
            string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            var seen = new List<string>();
            foreach (var (name, place) in new[] { ("Home", home), ("Desktop", Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory)),
                         ("Documents", Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments)), ("Downloads", Path.Combine(home, "Downloads")) })
            {
                if (string.IsNullOrEmpty(place) || !Directory.Exists(place) || seen.Any(p => Same(p, place))) continue;
                seen.Add(place);
                page.Children.Add(new ActionRow(name, () => Show(place, null), "Open", place) { Tag = place });
            }
            Heading("Drives");
            foreach (var (drive, about) in Drives())
                page.Children.Add(new ActionRow(drive, () => Show(drive, null), "Open", drive) { Value = about, Tag = drive });
            Heading("Elsewhere");
            page.Children.Add(new ActionRow("Type a path", () => TypePath().Watch(), "Type", "Type the path of a folder" + (folder ? "" : " or a file") + "."));
        }

        void Heading(string text, string? hint = null)
        {
            var h = new MenuHeading(text, hint);
            if (page.Children.Count == 0) h.Margin = new Thickness(0, 0, 0, 2);
            page.Children.Add(h);
        }

        // The selection after a change: the row for `key` (a path or a label), else the first folder or file, else the first.
        void Reselect(string? key)
        {
            dialog.UpdateLayout();
            var rows = screen.Rows.ToList();
            var row = (key == null ? null : rows.FirstOrDefault(r => r.Tag is string t ? Same(t, key) : r.Label == key))
                      ?? rows.FirstOrDefault(r => r.Tag is string) ?? rows.FirstOrDefault();
            screen.Scroller.Offset = default;
            screen.Select(row);
        }

        void Up()
        {
            if (current == null) return;
            Show(Directory.GetParent(current)?.FullName, current);
        }

        async Task Pick(string file)
        {
            if (!File.Exists(file)) { await Dialogs.Message(dialog, "The file no longer exists."); return; }
            answer = file;
            dialog.Close();
        }

        async Task UseFolder()
        {
            if (current == null || !Directory.Exists(current)) { await Dialogs.Message(dialog, "The folder no longer exists."); return; }
            answer = current;
            dialog.Close();
        }

        async Task TypePath()
        {
            var typed = await Text(dialog, folder ? "Type a folder's path" : "Type a folder's or a file's path", current ?? "");
            if (string.IsNullOrWhiteSpace(typed)) return;
            typed = typed.Trim().Trim('"');
            if (!folder && File.Exists(typed)) { await Pick(typed); return; }
            if (!Directory.Exists(typed)) { await Dialogs.Message(dialog, "There's no folder at " + typed + "."); return; }
            Show(typed, null);
        }

        async Task NewFolder()
        {
            if (current == null) return;
            var name = await Text(dialog, "New folder name", "");
            if (name == null) return;
            if (string.IsNullOrWhiteSpace(name) || name is "." or ".." || name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 ||
                name.Contains(Path.DirectorySeparatorChar) || name.Contains(Path.AltDirectorySeparatorChar))
            { await Dialogs.Message(dialog, "Enter a single valid folder name."); return; }
            try
            {
                string made = Directory.CreateDirectory(Path.Combine(current, name)).FullName;
                Show(current, made);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException)
            { await Dialogs.Message(dialog, "Couldn't create the folder: " + e.Message); }
        }
    }

    static bool Same(string a, string b) => string.Equals(Path.TrimEndingDirectorySeparator(a), Path.TrimEndingDirectorySeparator(b),
        OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal);

    // Not hidden or a system folder (AppData, $Recycle.Bin; on Linux the dot folders).
    static bool Shown(string dir)
    {
        try { return (File.GetAttributes(dir) & (FileAttributes.Hidden | FileAttributes.System)) == 0; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return false; }
    }

    static string Size(string file)
    {
        try { return Size(new FileInfo(file).Length); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return ""; }
    }

    static string Size(long bytes) => bytes >= 1L << 30 ? (bytes / (double)(1L << 30)).ToString("0.0", CultureInfo.CurrentCulture) + " GB"
        : bytes >= 1L << 20 ? (bytes >> 20) + " MB" : Math.Max(1, bytes >> 10) + " KB";

    // The drives to offer: every ready one on Windows; elsewhere / and the mounts under /media, /run/media and /mnt (not
    // /proc, /sys and the like).
    static List<(string Root, string About)> Drives()
    {
        var list = new List<(string, string)>();
        foreach (var d in DriveInfo.GetDrives())
        {
            string root = d.RootDirectory.FullName;
            if (!OperatingSystem.IsWindows() && root != "/" && !root.StartsWith("/media/") && !root.StartsWith("/run/media/") && !root.StartsWith("/mnt/"))
                continue;
            try
            {
                if (!d.IsReady) continue;
                string label = d.VolumeLabel is { Length: > 0 } v && v != root ? v + ", " : "";
                list.Add((root, label + Size(d.TotalSize)));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
        return list;
    }
}

sealed class ControllerTextBox : TextBox
{
    protected override Type StyleKeyOverride => typeof(TextBox);
}
