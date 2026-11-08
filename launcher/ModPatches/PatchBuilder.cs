using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace FrightWorks.ModPatches;

enum PatchState { Off, Ready, Problem }

sealed record ModPatchStatus(PatchState State, string Text);

sealed class PatchResult
{
    public readonly Dictionary<string, ModPatchStatus> Mods = new(StringComparer.OrdinalIgnoreCase);  // by mod folder name; a mod with no patch.json isn't here
    public bool Rebuilt;
    public string? Error;  // something unexpected: the generated folder was removed and the game starts unmodded
}

// Builds the level files patch mods change, from the player's own disc, into mods/!patched. The game reads that like any
// other mod (riivolution.cpp), so a player who starts it without the launcher gets the last build. "!" sorts before
// ordinary names, so a mod that replaces a whole level beats a patch to it. Rule: a problem costs that mod, never the game.
// Nothing here is shown or thrown at the caller: Ensure returns a result whatever happens, so it can't block Play.
static class PatchBuilder
{
    public const string OutputFolder = "!patched";
    const string StampFile = "build.json";
    const int ToolVersion = 1;  // bump when a build's bytes would change: updates rebuild
    const string StagingName = "mod-build";

    // Added model and texture bytes per level, over all mods. Each hub's limit is no more than a size that ran there: drew,
    // kept 60 fps and came back after a level change. That is what was tried, not the most that fits: h002 also ran with
    // 1,554,672 bytes, h001 with 1,085,512, h003 with 267,756. Models come out of the OS heap at about 0.9 of their file size,
    // and these hubs have about 1.85, 1.41 and 0.60 MB free. Other levels haven't been measured, so they get the smallest limit.
    const long DefaultBudget = 256 * 1024;
    static readonly Dictionary<string, long> Budgets = new() { ["h001"] = 1024 * 1024, ["h002"] = 1024 * 1024, ["h003"] = 256 * 1024 };

    static readonly object Gate = new();

    public static PatchResult Ensure(string modsDir, string discDir, string cacheDir, Func<bool>? isGameRunning = null)
    {
        lock (Gate)
        {
            var result = new PatchResult();
            bool gameRunning = false;
            try
            {
                gameRunning = isGameRunning?.Invoke() ?? false;  // asked here, not by the caller: the game may have started while this waited for the lock
                Run(result, modsDir, discDir, cacheDir, gameRunning);
            }
            catch (Exception e)
            {
                if (!gameRunning) RemoveOutput(modsDir);
                result.Error = e.Message;
                foreach (var key in result.Mods.Keys.ToList())
                    if (result.Mods[key].State == PatchState.Ready) result.Mods[key] = new ModPatchStatus(PatchState.Problem, "Not applied: " + e.Message);
            }
            finally { TryDelete(Path.Combine(cacheDir, StagingName)); }
            return result;
        }
    }

    sealed class LoadedMod
    {
        public required string Folder, Dir;
        public required PatchRecipe Recipe;
        public required Dictionary<string, byte[]> Files;
    }

    sealed class Disc
    {
        readonly string files;
        readonly Dictionary<string, string> paths = new(StringComparer.OrdinalIgnoreCase);
        readonly Dictionary<string, (byte[] Bytes, string Sha)> read = new();
        readonly Dictionary<string, HipFile> parsed = new();

        public Disc(string discDir)
        {
            files = Path.Combine(discDir, "files");
            if (!Directory.Exists(files)) return;
            foreach (var f in Directory.EnumerateFiles(files, "*.hip", new EnumerationOptions { RecurseSubdirectories = true, MatchCasing = MatchCasing.CaseInsensitive }))
                paths[Path.GetFileNameWithoutExtension(f)] = f;
        }

        public bool Has(string level) => paths.ContainsKey(level);

        // Relative to the disc's files folder with the disc's own spelling: Linux needs the exact case.
        public string Relative(string level) => Path.GetRelativePath(files, paths[level]);

        public (byte[] Bytes, string Sha) Read(string level)
        {
            if (!read.TryGetValue(level, out var r))
            {
                var bytes = File.ReadAllBytes(paths[level]);
                read[level] = r = (bytes, Convert.ToHexString(SHA256.HashData(bytes)));
            }
            return r;
        }

        public HipFile Parse(string level)
        {
            if (!parsed.TryGetValue(level, out var h)) parsed[level] = h = new HipFile(Read(level).Bytes);
            return h;
        }

        public IEnumerable<string> ReadLevels => read.Keys.OrderBy(k => k, StringComparer.Ordinal);
    }

    static void Run(PatchResult result, string modsDir, string discDir, string cacheDir, bool gameRunning)
    {
        var output = Path.Combine(modsDir, OutputFolder);
        var dirs = Directory.Exists(modsDir)
            ? Directory.GetDirectories(modsDir).Where(d => !Path.GetFileName(d).Equals(OutputFolder, StringComparison.OrdinalIgnoreCase)).OrderBy(d => d, StringComparer.OrdinalIgnoreCase).ToArray()
            : Array.Empty<string>();
        var disc = new Disc(discDir);
        var loaded = new List<LoadedMod>();
        var stamp = new StringBuilder($"tool {ToolVersion}\n");

        foreach (var dir in dirs)
        {
            var folder = Path.GetFileName(dir);
            if (!File.Exists(Path.Combine(dir, PatchRecipe.FileName))) continue;
            if (File.Exists(Path.Combine(dir, "disabled"))) { result.Mods[folder] = new ModPatchStatus(PatchState.Off, ""); continue; }
            try
            {
                stamp.Append("mod ").Append(folder).Append(' ').Append(Sha(ModFiles.ReadRecipe(dir))).Append('\n');
                var (recipe, files) = ModFiles.Load(dir);
                foreach (var f in files.OrderBy(f => f.Key, StringComparer.Ordinal)) stamp.Append(f.Key).Append(' ').Append(Sha(f.Value)).Append('\n');
                CheckAgainstDisc(recipe, disc);
                loaded.Add(new LoadedMod { Folder = folder, Dir = dir, Recipe = recipe, Files = files });
                result.Mods[folder] = new ModPatchStatus(PatchState.Ready, "Ready.");
            }
            catch (Exception e) when (IsModProblem(e)) { stamp.Append("problem ").Append(e.Message).Append('\n'); result.Mods[folder] = new ModPatchStatus(PatchState.Problem, e.Message); }
        }
        foreach (var level in disc.ReadLevels) stamp.Append("disc ").Append(level).Append(' ').Append(disc.Read(level).Sha).Append('\n');
        var wanted = Sha(Encoding.UTF8.GetBytes(stamp.ToString()));

        bool exists = Directory.Exists(output), ours = exists && IsOurs(output);
        if (exists && !ours)
        {
            // Not something this code made: leave it alone, and say so.
            foreach (var m in loaded) result.Mods[m.Folder] = new ModPatchStatus(PatchState.Problem, $"Not applied: the mods folder has a folder called {OutputFolder} that FrightWorks didn't make. Rename it.");
            return;
        }

        if (ours && ReadStamp(output) is { } done && done.Stamp == wanted && done.Files.All(f => File.Exists(Path.Combine(output, f))))
        {
            foreach (var (folder, text) in done.Problems) result.Mods[folder] = new ModPatchStatus(PatchState.Problem, text);
            AddOverrideWarnings(result, loaded, disc, dirs);
            return;
        }
        if (loaded.Count == 0 && !exists) return;
        if (gameRunning)
        {
            foreach (var m in loaded) result.Mods[m.Folder] = new ModPatchStatus(PatchState.Ready, "Close the game: this change applies the next time you press Play.");
            return;
        }

        var levels = new Dictionary<string, LevelPlan>();
        var problems = new List<(string Folder, string Text)>();
        var applied = new List<LoadedMod>();
        foreach (var mod in loaded)
        {
            try { Plan(mod, disc, levels); applied.Add(mod); }
            catch (Exception e) when (IsModProblem(e)) { problems.Add((mod.Folder, e.Message)); result.Mods[mod.Folder] = new ModPatchStatus(PatchState.Problem, e.Message); }
        }

        var staging = Path.Combine(cacheDir, StagingName);
        TryDelete(staging);
        var stagedOutput = Path.Combine(staging, OutputFolder);
        var written = new List<string>();
        foreach (var plan in levels.Values.Where(p => p.Ops.Count > 0).OrderBy(p => p.Level, StringComparer.Ordinal))
        {
            var relative = Path.Combine("files", disc.Relative(plan.Level));
            var path = Path.Combine(stagedOutput, relative);
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllBytes(path, BuildLevel(plan, disc));
            written.Add(relative);
        }

        if (written.Count > 0)
        {
            WriteStamp(Path.Combine(stagedOutput, StampFile), wanted, written, problems);
            // Everything is built and checked. Deleting then moving: if power fails in between there's no folder and the
            // game starts unmodded, never a half-written level. Staging stays outside mods/ so the game can't see it.
            if (exists) Directory.Delete(output, true);
            Directory.CreateDirectory(modsDir);
            Directory.Move(stagedOutput, output);
            result.Rebuilt = true;
        }
        else if (exists) { Directory.Delete(output, true); result.Rebuilt = true; }
        AddOverrideWarnings(result, applied, disc, dirs);
    }

    // What should cost only the mod it happened in: a bad mod, or a file that can't be read while handling it.
    static bool IsModProblem(Exception e) => e is PatchException or InvalidDataException or IOException or UnauthorizedAccessException;

    static void CheckAgainstDisc(PatchRecipe recipe, Disc disc)
    {
        foreach (var level in recipe.Levels)
        {
            if (!disc.Has(level.Level)) throw new PatchException($"Your game disc has no level called {level.Level}.");
            if (disc.Read(level.Level).Sha != level.BaseSha) throw new PatchException($"This mod was made for a different copy of the game: your {level.Level} doesn't match.");
            foreach (var asset in level.Add)
            {
                if (asset.FromLevel == null) continue;
                if (!disc.Has(asset.FromLevel)) throw new PatchException($"Your game disc has no level called {asset.FromLevel}.");
                disc.Read(asset.FromLevel);
            }
        }
    }

    sealed record Op(uint Layer, string Type, uint Id, ReadOnlyMemory<byte> Data, string Name, uint Flags, uint Align);

    sealed class LevelPlan
    {
        public required string Level;
        public readonly List<Op> Ops = new();
        public readonly Dictionary<uint, (string Owner, ReadOnlyMemory<byte> Data)> Added = new();
        public long Used;
        public int Objects;
    }

    // Works out one mod's changes without touching anything, and commits them only if all of them fit: all or nothing.
    static void Plan(LoadedMod mod, Disc disc, Dictionary<string, LevelPlan> levels)
    {
        var commits = new List<Action>();
        foreach (var patch in mod.Recipe.Levels)
        {
            if (!levels.TryGetValue(patch.Level, out var plan)) plan = new LevelPlan { Level = patch.Level };
            var committed = plan;
            var hip = disc.Parse(patch.Level);
            var ops = new List<Op>();
            var mine = new Dictionary<uint, ReadOnlyMemory<byte>>();
            var models = new Dictionary<string, uint>(StringComparer.OrdinalIgnoreCase);
            long bytes = 0;

            // Same ID and same bytes as what's there is shared; different bytes is a clash, and this mod is the one skipped.
            bool NeedsAdding(uint id, ReadOnlyMemory<byte> data, string what)
            {
                if (mine.TryGetValue(id, out var own))
                {
                    if (own.Span.SequenceEqual(data.Span)) return false;
                    throw new PatchException($"{patch.Level}: this mod adds {what} ({id:X8}) twice with different contents.");
                }
                if (hip.Find(id) is { } existing)
                {
                    if (existing.Data.Span.SequenceEqual(data.Span)) return false;
                    throw new PatchException($"{patch.Level}: {what} ({id:X8}) clashes with an asset the level already has.");
                }
                if (committed.Added.TryGetValue(id, out var other))
                {
                    if (other.Data.Span.SequenceEqual(data.Span)) return false;
                    throw new PatchException($"Skipped: {what} ({id:X8}) in {patch.Level} clashes with the mod \"{other.Owner}\", which comes first.");
                }
                mine[id] = data;
                return true;
            }

            foreach (var asset in patch.Add)
            {
                ReadOnlyMemory<byte> data;
                uint id;
                string name;
                if (asset.File != null)
                {
                    data = mod.Files[asset.File];
                    name = asset.Name!;
                    id = HipFile.HashName(name);
                }
                else
                {
                    var source = disc.Parse(asset.FromLevel!).Find(asset.FromId) ?? throw new PatchException($"{asset.FromLevel} has no asset {asset.FromId:X8}.");
                    if (source.TypeName != asset.Type) throw new PatchException($"Asset {asset.FromId:X8} in {asset.FromLevel} is a {source.TypeName}, not a {asset.Type}.");
                    if (Convert.ToHexString(SHA256.HashData(source.Data.Span)) != asset.FromSha) throw new PatchException($"This mod was made for a different copy of the game: asset {asset.FromId:X8} in your {asset.FromLevel} doesn't match.");
                    data = source.Data;
                    name = source.Name;
                    id = source.Id;
                }
                if (asset.Type == "MODL") models[name] = id;
                if (!NeedsAdding(id, data, name)) continue;
                bytes += data.Length;
                ops.Add(asset.Type == "MODL"
                    ? new Op(HipFile.LayerModels, "MODL", id, data, name, 0x5, 0x10)
                    : new Op(HipFile.LayerTextures, "RWTX", id, data, name, 0x6, 0x40));
            }

            int objects = 0;
            if (patch.Objects.Count > 0)
            {
                var simples = hip.Assets.Where(a => a.TypeName == "SIMP" && a.Data.Length == SimpleSize).ToList();
                if (simples.Count == 0) throw new PatchException($"{patch.Level} has no simple object to copy the layout from.");
                var template = (simples.FirstOrDefault(a => a.Name == "TREE1") ?? simples[0]).Data;
                foreach (var obj in patch.Objects)
                {
                    uint model;
                    if (models.TryGetValue(obj.Model, out var m)) model = m;
                    else model = hip.Assets.FirstOrDefault(a => a.TypeName == "MODL" && a.Name.Equals(obj.Model, StringComparison.OrdinalIgnoreCase))?.Id
                                 ?? throw new PatchException($"{patch.Level}: object {obj.Name} uses the model {obj.Model}, which this mod doesn't add and the level doesn't have.");
                    var id = HipFile.HashName(obj.Name);
                    var data = MakeSimple(template.Span, id, obj, model);
                    if (!NeedsAdding(id, data, obj.Name)) continue;
                    objects++;
                    ops.Add(new Op(HipFile.LayerDefault, "SIMP", id, data, obj.Name, 0x2, 0x20));
                }
            }
            foreach (var op in ops)
                if (!hip.HasLayer(op.Layer)) throw new PatchException($"{patch.Level} has no place for a {op.Type}.");

            long budget = Budgets.GetValueOrDefault(patch.Level, DefaultBudget);
            if (plan.Used + bytes > budget) throw new PatchException($"Skipped: too big for {patch.Level}. It would add {(plan.Used + bytes) / 1024} KB of models and textures there, and the limit is {budget / 1024} KB.");
            if (plan.Objects + objects > PatchRecipe.MaxObjectsPerLevel) throw new PatchException($"Skipped: too many objects for {patch.Level} (the limit is {PatchRecipe.MaxObjectsPerLevel}).");

            commits.Add(() =>
            {
                levels[patch.Level] = committed;
                committed.Ops.AddRange(ops);
                foreach (var op in ops) committed.Added[op.Id] = (mod.Folder, op.Data);
                committed.Used += bytes;
                committed.Objects += objects;
            });
        }
        foreach (var commit in commits) commit();
    }

    const int SimpleSize = 0x5C;

    // +0 ID, +0x10 yaw in radians then two more angles, +0x1C position, +0x28 scale, +0x34 colour multipliers,
    // +0x40 see-through (1 = never fades), +0x48 model ID, +0x58 collision type (2 solid, 0 walk-through). The rest as the template has it.
    static byte[] MakeSimple(ReadOnlySpan<byte> template, uint id, PatchObject o, uint model)
    {
        var s = template.ToArray();
        BinaryPrimitives.WriteUInt32BigEndian(s.AsSpan(0), id);
        PutFloats(s, 0x10, o.Yaw, 0, 0);
        PutFloats(s, 0x1C, o.Position);
        PutFloats(s, 0x28, o.Scale);
        PutFloats(s, 0x34, o.Tint);
        PutFloats(s, 0x40, o.SeeThrough);
        BinaryPrimitives.WriteUInt32BigEndian(s.AsSpan(0x48), model);
        s[0x58] = o.Solid ? (byte)2 : (byte)0;
        return s;
    }

    static void PutFloats(byte[] d, int o, params float[] values)
    {
        for (int i = 0; i < values.Length; i++) BinaryPrimitives.WriteSingleBigEndian(d.AsSpan(o + 4 * i), values[i]);
    }

    // Builds from the disc's bytes, then reads the result back: every original and added asset must be there, in ID order.
    static byte[] BuildLevel(LevelPlan plan, Disc disc)
    {
        var hip = new HipFile(disc.Read(plan.Level).Bytes);
        foreach (var op in plan.Ops) hip.Add(op.Layer, op.Type, op.Id, op.Data, op.Name, op.Flags, op.Align);
        var built = hip.Build();
        var check = new HipFile(built);
        var original = disc.Parse(plan.Level);
        if (check.Assets.Count != original.Assets.Count + plan.Ops.Count) throw new InvalidDataException($"the rebuilt {plan.Level} has the wrong number of assets");
        for (int i = 1; i < check.Assets.Count; i++)
            if (check.Assets[i - 1].Id >= check.Assets[i].Id) throw new InvalidDataException($"the rebuilt {plan.Level} is out of order");
        foreach (var a in original.Assets)
            if (check.Find(a.Id) is not { } again || !again.Data.Span.SequenceEqual(a.Data.Span)) throw new InvalidDataException($"the rebuilt {plan.Level} changed an asset");
        foreach (var op in plan.Ops)
            if (check.Find(op.Id) is not { } again || !again.Data.Span.SequenceEqual(op.Data.Span)) throw new InvalidDataException($"the rebuilt {plan.Level} lost an added asset");
        return built;
    }

    // A mod replacing a whole level file beats a patch to that level: say so on the patch mod's line.
    static void AddOverrideWarnings(PatchResult result, List<LoadedMod> mods, Disc disc, string[] dirs)
    {
        foreach (var mod in mods)
        {
            if (result.Mods[mod.Folder].State != PatchState.Ready) continue;
            foreach (var level in mod.Recipe.Levels)
            {
                var relative = Path.Combine("files", disc.Relative(level.Level));
                var over = dirs.FirstOrDefault(d => !File.Exists(Path.Combine(d, "disabled")) && File.Exists(Path.Combine(d, relative)));
                if (over != null)
                    result.Mods[mod.Folder] = new ModPatchStatus(PatchState.Ready, $"Ready, but \"{Path.GetFileName(over)}\" replaces {level.Level}, so this mod's changes to that level won't show.");
            }
        }
    }

    static string Sha(byte[] data) => Convert.ToHexString(SHA256.HashData(data));

    static bool IsOurs(string output) =>
        !new DirectoryInfo(output).Attributes.HasFlag(FileAttributes.ReparsePoint) && File.Exists(Path.Combine(output, StampFile)) && ReadStamp(output) != null;

    static void WriteStamp(string path, string stamp, List<string> files, List<(string Folder, string Text)> problems)
    {
        using var stream = File.Create(path);
        using var w = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true });
        w.WriteStartObject();
        w.WriteString("note", "Made by the FrightWorks launcher from your own game disc. Holds game data, so it must not be shared.");
        w.WriteString("stamp", stamp);
        w.WriteStartArray("files");
        foreach (var f in files) w.WriteStringValue(f.Replace('\\', '/'));
        w.WriteEndArray();
        w.WriteStartObject("problems");
        foreach (var (folder, text) in problems) w.WriteString(folder, text);
        w.WriteEndObject();
        w.WriteEndObject();
    }

    static (string Stamp, List<string> Files, List<(string Folder, string Text)> Problems)? ReadStamp(string output)
    {
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(output, StampFile)));
            var root = doc.RootElement;
            var files = root.GetProperty("files").EnumerateArray().Select(f => (f.GetString() ?? "").Replace('/', Path.DirectorySeparatorChar)).ToList();
            var problems = root.GetProperty("problems").EnumerateObject().Select(p => (p.Name, p.Value.GetString() ?? "")).ToList();
            return (root.GetProperty("stamp").GetString() ?? "", files, problems);
        }
        catch (Exception e) when (e is JsonException or IOException or InvalidOperationException or KeyNotFoundException) { return null; }
    }

    static void RemoveOutput(string modsDir)
    {
        var output = Path.Combine(modsDir, OutputFolder);
        try { if (Directory.Exists(output) && IsOurs(output)) Directory.Delete(output, true); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }

    static void TryDelete(string dir)
    {
        try { if (Directory.Exists(dir)) Directory.Delete(dir, true); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }
}
