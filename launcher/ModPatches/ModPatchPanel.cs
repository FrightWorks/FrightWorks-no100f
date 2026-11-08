using Avalonia.Controls;
using Avalonia.Threading;

namespace FrightWorks.ModPatches;

// What the launcher's window does with patch mods: the status under a mod on the Mods page, the Add mod button, and the
// build before Play. In its own file so MainWindow only needs a few calls.
sealed class ModPatchPanel
{
    readonly Window owner;
    readonly DataPaths paths;
    readonly Func<bool> gameRunning;
    PatchResult last = new();

    public ModPatchPanel(Window owner, DataPaths paths, Func<bool> gameRunning) => (this.owner, this.paths, this.gameRunning) = (owner, paths, gameRunning);

    string CacheDir => Path.Combine(paths.UserData, "Cache");

    // The generated folder isn't a mod the player can tick.
    public static bool IsListed(string modDir) => !Path.GetFileName(modDir).Equals(PatchBuilder.OutputFolder, StringComparison.OrdinalIgnoreCase);

    // The line under a mod, or null when there's nothing to say.
    public string? StatusText(string modDir) => last.Mods.TryGetValue(Path.GetFileName(modDir), out var s) && s.Text.Length > 0 ? s.Text : null;

    Task<PatchResult> Build()
    {
        var (mods, disc, cache) = (paths.Mods, paths.PlayableDisc, CacheDir);
        return Task.Run(() => PatchBuilder.Ensure(mods, disc, cache, gameRunning));
    }

    // After a tick, an added mod or Refresh: builds, then redraws the list with the new statuses.
    public async Task Update(Action redraw)
    {
        last = await Build();
        Dispatcher.UIThread.Post(redraw);
    }

    // Before Play. Never stops the game from starting: at worst it starts unmodded, and the player is told why.
    public async Task BeforePlay()
    {
        last = await Build();
        if (last.Error != null)
            await Dialogs.Message(owner, "The mods that change levels couldn't be applied, so the game starts without them.\n\n" + last.Error);
        else if (last.Rebuilt && last.Mods.Where(m => m.Value.State == PatchState.Problem).Select(m => $"{m.Key}: {m.Value.Text}").ToList() is { Count: > 0 } problems)
            await Dialogs.Message(owner, "Some mods weren't applied. The game starts without them.\n\n" + string.Join("\n\n", problems));
    }

    // The Add mod button: choose a zip, check it, ask before replacing, install.
    public async Task AddFromZip(Action redraw)
    {
        // Themed browser when the pad started this (decided inside PickPath before any await), native picker otherwise.
        var path = await ControllerDialogs.PickPath(owner, "Choose a mod (.zip)", extensions: new[] { ".zip" }, typeName: "Mod zip");
        if (path == null) return;
        var (mods, cache) = (paths.Mods, CacheDir);
        ModZip.Prepared mod;
        try { mod = await Task.Run(() => ModZip.Prepare(path, cache)); }
        catch (PatchException e) { await Dialogs.Message(owner, "That mod wasn't added. The mods folder is unchanged.\n\n" + e.Message); return; }
        if (ModZip.Exists(mod, mods) && !await Dialogs.YesNo(owner, $"You already have a mod called \"{mod.Name}\". Replace it?"))
        {
            ModZip.Discard(mod);
            return;
        }
        if (gameRunning())
        {
            ModZip.Discard(mod);
            await Dialogs.Message(owner, "Close the game first: it is using the mods.");
            return;
        }
        try { await Task.Run(() => ModZip.Commit(mod, mods)); }
        catch (PatchException e) { await Dialogs.Message(owner, "That mod wasn't added.\n\n" + e.Message); return; }
        redraw();
        await Update(redraw);
    }
}
