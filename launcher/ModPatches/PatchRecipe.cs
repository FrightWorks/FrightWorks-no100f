using System.Buffers.Binary;
using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace FrightWorks.ModPatches;

// A problem with one mod. The text is shown on the Mods page, so it's written for the player.
sealed class PatchException : Exception
{
    public PatchException(string message) : base(message) { }
}

sealed record PatchAsset(string Type, string? Name, string? File, string? FromLevel, uint FromId, string? FromSha);

sealed record PatchObject(string Name, string Model, float[] Position, float Yaw, float[] Scale, float[] Tint, float SeeThrough, bool Solid);

sealed record LevelPatch(string Level, string BaseSha, List<PatchAsset> Add, List<PatchObject> Objects);

// patch.json. Mods are untrusted downloads: this is data only, everything is checked, and anything unknown is refused.
// The format is in docs/mod-patches.md.
sealed record PatchRecipe(string? DisplayName, List<LevelPatch> Levels)
{
    public const string FileName = "patch.json";
    public const int MaxRecipeBytes = 1 << 20, MaxObjectsPerLevel = 32, MaxAssetBytes = 8 << 20;
    const int MaxLevels = 16, MaxAssetsPerLevel = 64, MaxNameLength = 63;

    static readonly Regex LevelName = new("^[A-Za-z0-9_]{1,16}$", RegexOptions.CultureInvariant);
    static readonly Regex Sha = new("^[0-9A-Fa-f]{64}$", RegexOptions.CultureInvariant);
    static readonly Regex Id = new("^[0-9A-Fa-f]{8}$", RegexOptions.CultureInvariant);

    public static PatchRecipe Parse(byte[] json)
    {
        if (json.Length > MaxRecipeBytes) throw new PatchException($"{FileName} is too large.");
        JsonDocument doc;
        try { doc = JsonDocument.Parse(json); }
        catch (JsonException e) { throw new PatchException($"{FileName} isn't valid JSON: {e.Message}"); }
        using (doc)
        {
            var root = doc.RootElement;
            Fields(root, "the file", "format", "name", "author", "description", "levels");
            if (!root.TryGetProperty("format", out var format) || format.ValueKind != JsonValueKind.Number || !format.TryGetInt32(out var f))
                throw new PatchException($"{FileName} has no format number.");
            if (f != 1) throw new PatchException($"This mod uses patch format {f}, and this version of FrightWorks only knows format 1. A newer FrightWorks may be needed.");
            if (!root.TryGetProperty("levels", out var levels) || levels.ValueKind != JsonValueKind.Object) throw new PatchException($"{FileName} has no levels.");
            var result = new List<LevelPatch>();
            foreach (var level in levels.EnumerateObject())
            {
                if (!LevelName.IsMatch(level.Name)) throw new PatchException($"\"{level.Name}\" isn't a level name.");
                if (result.Any(l => l.Level.Equals(level.Name, StringComparison.OrdinalIgnoreCase))) throw new PatchException($"Level {level.Name} is listed twice.");
                result.Add(ParseLevel(level.Name.ToLowerInvariant(), level.Value));
            }
            if (result.Count == 0) throw new PatchException($"{FileName} lists no levels.");
            if (result.Count > MaxLevels) throw new PatchException($"{FileName} lists too many levels.");
            string? display = root.TryGetProperty("name", out var n) && n.ValueKind == JsonValueKind.String ? n.GetString() : null;
            return new PatchRecipe(display, result);
        }
    }

    static LevelPatch ParseLevel(string level, JsonElement e)
    {
        Fields(e, level, "base_sha256", "add", "objects");
        var sha = Text(e, "base_sha256", level);
        if (!Sha.IsMatch(sha)) throw new PatchException($"{level}: base_sha256 isn't a SHA-256.");
        var add = new List<PatchAsset>();
        if (e.TryGetProperty("add", out var list))
        {
            if (list.ValueKind != JsonValueKind.Array) throw new PatchException($"{level}: add isn't a list.");
            if (list.GetArrayLength() > MaxAssetsPerLevel) throw new PatchException($"{level}: too many added assets.");
            foreach (var a in list.EnumerateArray()) add.Add(ParseAsset(level, a));
        }
        var objects = new List<PatchObject>();
        if (e.TryGetProperty("objects", out var objs))
        {
            if (objs.ValueKind != JsonValueKind.Array) throw new PatchException($"{level}: objects isn't a list.");
            if (objs.GetArrayLength() > MaxObjectsPerLevel) throw new PatchException($"{level}: more than {MaxObjectsPerLevel} objects.");
            foreach (var o in objs.EnumerateArray()) objects.Add(ParseObject(level, o));
        }
        if (add.Count == 0 && objects.Count == 0) throw new PatchException($"{level}: nothing to add.");
        return new LevelPatch(level, sha.ToUpperInvariant(), add, objects);
    }

    static PatchAsset ParseAsset(string level, JsonElement e)
    {
        Fields(e, level + " asset", "type", "name", "file", "from");
        var type = Text(e, "type", level);
        if (type is not ("RWTX" or "MODL")) throw new PatchException($"{level}: an added asset must be RWTX or MODL, not {type}.");
        bool hasFile = e.TryGetProperty("file", out _), hasFrom = e.TryGetProperty("from", out var from);
        if (hasFile == hasFrom) throw new PatchException($"{level}: an added asset needs either file or from.");
        if (hasFile)
        {
            var name = CheckName(Text(e, "name", level), level);
            if (type == "RWTX" && !name.EndsWith(".RW3", StringComparison.OrdinalIgnoreCase)) throw new PatchException($"{level}: texture \"{name}\" must be named NAME.RW3, because materials find textures by that name.");
            var path = Text(e, "file", level);
            ModFiles.CheckRelativePath(path);
            return new PatchAsset(type, name, path, null, 0, null);
        }
        Fields(from, level + " from", "level", "id", "sha256");
        var source = Text(from, "level", level);
        if (!LevelName.IsMatch(source)) throw new PatchException($"{level}: \"{source}\" isn't a level name.");
        var id = Text(from, "id", level);
        if (!Id.IsMatch(id)) throw new PatchException($"{level}: \"{id}\" isn't an asset ID (8 hex digits).");
        var sha = Text(from, "sha256", level);
        if (!Sha.IsMatch(sha)) throw new PatchException($"{level}: from.sha256 isn't a SHA-256.");
        return new PatchAsset(type, null, null, source.ToLowerInvariant(), uint.Parse(id, NumberStyles.HexNumber, CultureInfo.InvariantCulture), sha.ToUpperInvariant());
    }

    static PatchObject ParseObject(string level, JsonElement e)
    {
        Fields(e, level + " object", "name", "model", "pos", "yaw", "scale", "tint", "see_through", "solid");
        var name = CheckName(Text(e, "name", level), level);
        var model = CheckName(Text(e, "model", level), level);
        var pos = Vector(e, "pos", level, name, null, -100000, 100000) ?? throw new PatchException($"{level}: object {name} has no pos.");
        double yaw = Number(e, "yaw", level, name, 0, -100000, 100000);
        var scale = Vector(e, "scale", level, name, 1, 0.001, 1000) ?? new[] { 1f, 1f, 1f };
        var tint = Vector(e, "tint", level, name, 1, 0, 16) ?? new[] { 1f, 1f, 1f };
        double see = Number(e, "see_through", level, name, 1, 0, 1);
        bool solid = false;
        if (e.TryGetProperty("solid", out var s))
        {
            if (s.ValueKind is not (JsonValueKind.True or JsonValueKind.False)) throw new PatchException($"{level}: object {name}: solid must be true or false.");
            solid = s.GetBoolean();
        }
        return new PatchObject(name, model, pos, (float)(yaw * (Math.PI / 180.0)), scale, tint, (float)see, solid);
    }

    static string CheckName(string name, string level)
    {
        if (name.Length == 0 || name.Length > MaxNameLength || name.Any(c => c < 0x20 || c > 0x7E)) throw new PatchException($"{level}: \"{name}\" isn't a usable name (plain ASCII, up to {MaxNameLength} characters).");
        return name;
    }

    static void Fields(JsonElement e, string where, params string[] known)
    {
        if (e.ValueKind != JsonValueKind.Object) throw new PatchException($"{where} isn't an object.");
        var seen = new HashSet<string>();
        foreach (var p in e.EnumerateObject())
        {
            if (!known.Contains(p.Name)) throw new PatchException($"{where}: unknown setting \"{p.Name}\".");
            if (!seen.Add(p.Name)) throw new PatchException($"{where}: \"{p.Name}\" is listed twice.");
        }
    }

    static string Text(JsonElement e, string field, string where)
    {
        if (!e.TryGetProperty(field, out var v) || v.ValueKind != JsonValueKind.String) throw new PatchException($"{where}: {field} is missing or isn't text.");
        return v.GetString() ?? "";
    }

    static double Number(JsonElement e, string field, string level, string name, double fallback, double min, double max)
    {
        if (!e.TryGetProperty(field, out var v)) return fallback;
        if (v.ValueKind != JsonValueKind.Number || !v.TryGetDouble(out var d) || !double.IsFinite(d) || d < min || d > max) throw new PatchException($"{level}: object {name}: {field} must be a number from {min} to {max}.");
        return d;
    }

    static float[]? Vector(JsonElement e, string field, string level, string name, double? _, double min, double max)
    {
        if (!e.TryGetProperty(field, out var v)) return null;
        if (v.ValueKind != JsonValueKind.Array || v.GetArrayLength() != 3) throw new PatchException($"{level}: object {name}: {field} must be three numbers.");
        var r = new float[3];
        int i = 0;
        foreach (var x in v.EnumerateArray())
        {
            if (x.ValueKind != JsonValueKind.Number || !x.TryGetDouble(out var d) || !double.IsFinite(d) || d < min || d > max) throw new PatchException($"{level}: object {name}: {field} must hold numbers from {min} to {max}.");
            r[i++] = (float)d;
        }
        return r;
    }
}

// Paths and files inside a mod folder, which comes from someone else.
static class ModFiles
{
    static readonly string[] Reserved = { "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };

    // A relative path with forward slashes that can't leave the folder it's used in, and that means the same on Windows and Linux.
    public static void CheckRelativePath(string path)
    {
        if (path.Length == 0 || path.Length > 200 || path.StartsWith('/')) throw new PatchException($"\"{path}\" isn't a path inside the mod folder.");
        foreach (var part in path.Split('/')) CheckName(part, path);
    }

    public static void CheckName(string part, string path)
    {
        bool bad = part.Length == 0 || part is "." or ".." || part.EndsWith('.') || part.EndsWith(' ') || part.Any(c => c < 0x20 || "\\:*?\"<>|".Contains(c));
        var stem = part.Split('.')[0];
        if (bad || Reserved.Contains(stem, StringComparer.OrdinalIgnoreCase)) throw new PatchException($"\"{path}\" isn't a path inside the mod folder.");
    }

    // Reads a file of the mod after checking that no step of the way below the mod folder is a link.
    public static byte[] Read(string modDir, string relative, int maxBytes)
    {
        CheckRelativePath(relative);
        var full = modDir;
        foreach (var part in relative.Split('/'))
        {
            full = Path.Combine(full, part);
            if (!File.Exists(full) && !Directory.Exists(full)) throw new PatchException($"{relative} is missing from the mod.");
            if (new FileInfo(full).Attributes.HasFlag(FileAttributes.ReparsePoint)) throw new PatchException($"{relative} is a link, which mods can't use.");
        }
        if (!File.Exists(full)) throw new PatchException($"{relative} isn't a file.");
        var length = new FileInfo(full).Length;
        if (length > maxBytes) throw new PatchException($"{relative} is too large ({length / 1024} KB, the limit is {maxBytes / 1024} KB).");
        return File.ReadAllBytes(full);
    }

    public static byte[] ReadRecipe(string modDir) => Read(modDir, PatchRecipe.FileName, PatchRecipe.MaxRecipeBytes);

    // Parses a mod's recipe and loads and checks the author's own asset files. Touches nothing but the mod's folder.
    public static (PatchRecipe Recipe, Dictionary<string, byte[]> Files) Load(string modDir)
    {
        var recipe = PatchRecipe.Parse(ReadRecipe(modDir));
        var files = new Dictionary<string, byte[]>();
        foreach (var level in recipe.Levels)
            foreach (var asset in level.Add)
            {
                if (asset.File == null || files.ContainsKey(asset.File)) continue;
                var bytes = Read(modDir, asset.File, PatchRecipe.MaxAssetBytes);
                if (asset.Type == "MODL") RwCheck.Model(bytes, asset.File); else RwCheck.Texture(bytes, asset.File);
                files[asset.File] = bytes;
            }
        return (recipe, files);
    }
}

// Is this file shaped like what the game reads? Checks the file's structure, not every triangle, so a bad mesh can still
// pass: unticking the mod always recovers from that. Models on the disc are plain little-endian RenderWare 3.1 with no
// native data, and the largest has 12159 triangles; textures are a dictionary of GameCube native textures.
//
// A material with more than MaxMaterialVertices crashes the game: the draw's vertex count is 16 bits, so it wraps and the
// rest of the vertices are run as commands (seen in h002: 68,460 vertices, the display list's count said 2,924). The
// counts come from two places in a model, the geometry header and the per-material mesh list, and a file can make them
// disagree, so both are read and the larger one counts.
static class RwCheck
{
    public const int MaxTriangles = 13000;
    public const int MaxMaterialVertices = 65535;
    const uint Clump = 0x10, GeometryList = 0x1A, Geometry = 0x0F, Struct = 1, Extension = 3, MeshList = 0x50E, TextureDictionary = 0x16, TextureNative = 0x15;

    static IEnumerable<(uint Type, int Body, int Size, uint Version)> Sections(byte[] d, int start, int end, string file)
    {
        int o = start;
        while (o < end)
        {
            if (o + 12 > end) throw new PatchException($"{file} is cut short.");
            uint type = BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(o));
            uint size = BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(o + 4));
            uint version = BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(o + 8));
            if (size > (uint)(end - o - 12)) throw new PatchException($"{file} is cut short.");
            yield return (type, o + 12, (int)size, version);
            o += 12 + (int)size;
        }
    }

    public static void Model(byte[] d, string file)
    {
        // Only the clump and its geometry list are read: the disc's skinned models have an atomic after the list whose size
        // runs 12 bytes past the clump, and stray bytes after the clump, and the game loads them.
        var clump = Sections(d, 0, d.Length, file).FirstOrDefault();
        if (clump.Type != Clump || clump.Version != 0x310) throw new PatchException($"{file} isn't a model the game can read (it needs a RenderWare 3.1 clump).");
        long headerTriangles = 0, meshTriangles = 0;
        bool anyGeometry = false;
        foreach (var part in Sections(d, clump.Body, clump.Body + clump.Size, file))
        {
            if (part.Type != GeometryList) continue;
            foreach (var geometry in Sections(d, part.Body, part.Body + part.Size, file))
            {
                if (geometry.Type != Geometry) continue;
                var sections = Sections(d, geometry.Body, geometry.Body + geometry.Size, file).ToList();
                var first = sections.FirstOrDefault();
                if (first.Type != Struct || first.Size < 16) throw new PatchException($"{file} has a geometry with no header.");
                headerTriangles += BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(first.Body + 4));
                bool anyMeshList = false;
                foreach (var extension in sections.Where(s => s.Type == Extension))
                    foreach (var list in Sections(d, extension.Body, extension.Body + extension.Size, file).Where(s => s.Type == MeshList))
                    {
                        meshTriangles += MeshListTriangles(d, list.Body, list.Size, file);
                        anyMeshList = true;
                    }
                if (!anyMeshList) throw new PatchException($"{file} has a geometry with no material mesh list.");
                anyGeometry = true;
            }
            break;
        }
        if (!anyGeometry) throw new PatchException($"{file} has no geometry.");
        long triangles = Math.Max(headerTriangles, meshTriangles);
        if (triangles > MaxTriangles) throw new PatchException($"{file} has {triangles} triangles. The limit is {MaxTriangles}, just above the largest model in the game.");
    }

    // Flags (1 = triangle strip, 0 = triangle list), mesh count, total indices, then for each mesh its index count, its
    // material and its indices (4 bytes each). Returns how many triangles a list's meshes draw. A strip's index count isn't
    // a triangle count (it holds degenerate joins: a disc model has 19,672 by that sum, more than its header says), so
    // strips are only held to the vertex limit, which is what crashes the game.
    static long MeshListTriangles(byte[] d, int body, int size, string file)
    {
        if (size < 12) throw new PatchException($"{file} has a material mesh list that is cut short.");
        uint flags = BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(body));
        uint meshes = BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(body + 4));
        long o = body + 12, end = (long)body + size, triangles = 0;
        if (flags > 1) throw new PatchException($"{file} has a material mesh list of an unknown kind.");
        for (uint i = 0; i < meshes; i++)
        {
            if (o + 8 > end) throw new PatchException($"{file} has a material mesh list that is cut short.");
            uint indices = BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan((int)o));
            if (indices > MaxMaterialVertices) throw new PatchException($"{file} has a material with {indices} vertices. The game can draw at most {MaxMaterialVertices} with one material, and more crashes it.");
            o += 8 + 4L * indices;
            if (o > end) throw new PatchException($"{file} has a material mesh list that is cut short.");
            if (flags == 0) triangles += indices / 3;
        }
        return triangles;
    }

    public static void Texture(byte[] d, string file)
    {
        var top = Sections(d, 0, d.Length, file).ToList();
        if (top.Count == 0 || top[0].Type != TextureDictionary) throw new PatchException($"{file} isn't a texture the game can read (it needs a RenderWare texture dictionary).");
        int count = 0;
        foreach (var part in Sections(d, top[0].Body, top[0].Body + top[0].Size, file))
        {
            if (part.Type != TextureNative) continue;
            var first = Sections(d, part.Body, part.Body + part.Size, file).FirstOrDefault();
            // Big-endian GameCube header: platform 6, then at 0x58 raster, width, height, depth, mips, format, palette, reserved, data size.
            if (first.Type != Struct || first.Size < 0x6C || BinaryPrimitives.ReadUInt32BigEndian(d.AsSpan(first.Body)) != 6) throw new PatchException($"{file} isn't a GameCube texture.");
            int width = BinaryPrimitives.ReadUInt16BigEndian(d.AsSpan(first.Body + 0x5C)), height = BinaryPrimitives.ReadUInt16BigEndian(d.AsSpan(first.Body + 0x5E));
            uint bytes = BinaryPrimitives.ReadUInt32BigEndian(d.AsSpan(first.Body + 0x68));
            if (width is < 1 or > 1024 || height is < 1 or > 1024 || bytes > (uint)(first.Size - 0x6C)) throw new PatchException($"{file} has a texture with a bad size.");
            count++;
        }
        if (count == 0) throw new PatchException($"{file} holds no texture.");
    }
}
