using System.Buffers.Binary;
using System.IO.Compression;
using System.Text;
using HipPatchTests;
using FrightWorks.ModPatches;

// Tests for the launcher's mod patch code. With no arguments it needs nothing but dotnet: test data is made in code.
// The disc modes read a real disc, and whatever they write is game data, so give them git-ignored folders:
//   --disc DIR                      every level rebuilds byte for byte
//   --export DIR WORK               adds generated assets to every level; writes the hashes and the assets for reference.py
//   --recipe MOD --disc DIR --expect SHA [--level NAME]   builds one mod and compares the result
return args.Length > 0 && args[0] == "--export" ? DiscModes.Export(args[1], args[2])
     : args.Contains("--recipe") ? DiscModes.Recipe(args)
     : args.Length > 0 && args[0] == "--disc" ? DiscModes.RoundTrip(args[1])
     : Tests.RunAll();

static class Tests
{
    static int failed, passed;

    static void Check(string what, bool ok, string detail = "")
    {
        if (ok) { passed++; return; }
        failed++;
        Console.WriteLine($"FAIL {what}{(detail.Length > 0 ? ": " + detail : "")}");
    }

    public static int RunAll()
    {
        HipRoundTrip();
        HipAdd();
        Hash();
        Recipes();
        Builds();
        DiscFaults();
        Output();
        Zips();
        Console.WriteLine($"{passed} passed, {failed} failed");
        return failed == 0 ? 0 : 1;
    }

    static void HipRoundTrip()
    {
        var level = Fixtures.Level(1);
        Check("the made level rebuilds identically", new HipFile(level).Build().AsSpan().SequenceEqual(level));
        foreach (var bad in new[] { Array.Empty<byte>(), new byte[64], Encoding.ASCII.GetBytes("HIPA"), level[..100], level[..(level.Length / 3)] })
        {
            bool refused = false;
            try { new HipFile(bad); }
            catch (InvalidDataException) { refused = true; }
            Check($"a broken level of {bad.Length} bytes is refused", refused);
        }
    }

    static void HipAdd()
    {
        var hip = new HipFile(Fixtures.Level(1));
        int before = hip.Assets.Count;
        var model = Fixtures.Model("x");
        var texture = Fixtures.Texture("x");
        uint modelId = HipFile.HashName("x"), textureId = HipFile.HashName("x.RW3");
        hip.Add(HipFile.LayerTextures, "RWTX", textureId, texture, "x.RW3", 6, 0x40);
        hip.Add(HipFile.LayerModels, "MODL", modelId, model, "x", 5, 0x10);
        var back = new HipFile(hip.Build());
        Check("asset count grows", back.Assets.Count == before + 2);
        Check("added model reads back", back.Find(modelId)?.Data.Span.SequenceEqual(model) == true);
        Check("added texture reads back", back.Find(textureId)?.Data.Span.SequenceEqual(texture) == true);
        Check("added name reads back", back.Find(textureId)?.Name == "x.RW3");
        Check("texture is on 0x40", back.Find(textureId)!.Offset % 0x40 == 0, back.Find(textureId)!.Offset.ToString("X"));
        Check("model is on 0x10", back.Find(modelId)!.Offset % 0x10 == 0);
        for (int i = 1; i < back.Assets.Count; i++) Check("IDs stay sorted", back.Assets[i - 1].Id < back.Assets[i].Id);
        foreach (var a in new HipFile(Fixtures.Level(1)).Assets) Check($"{a.Name} is unchanged", back.Find(a.Id)?.Data.Span.SequenceEqual(a.Data.Span) == true);
        bool threw = false;
        try { hip.Add(HipFile.LayerModels, "MODL", modelId, model, "x", 5, 0x10); }
        catch (InvalidOperationException) { threw = true; }
        Check("an ID can't be added twice", threw);
    }

    static void Hash()
    {
        // Values from the game's rule as tested in game: our own names, and case doesn't matter.
        Check("hash fwhead", HipFile.HashName("fwhead") == 0xC70D9D85);
        Check("hash texture", HipFile.HashName("fwheadt0.RW3") == 0xDB9E951D);
        Check("hash object", HipFile.HashName("FWHEAD OBJ") == 0x44EAEA4C);
        Check("hash ignores case", HipFile.HashName("fwhead") == HipFile.HashName("FWHEAD"));
    }

    static void Recipes()
    {
        var sha = new string('A', 64);
        string Wrap(string level) => $$"""{ "format": 1, "levels": { "h002": {{level}} } }""";
        string Add(string add) => Wrap($$"""{ "base_sha256": "{{sha}}", "add": [ {{add}} ] }""");
        string Obj(string obj) => Wrap($$"""{ "base_sha256": "{{sha}}", "objects": [ {{obj}} ] }""");
        const string fine = """{ "name": "a", "model": "m", "pos": [1, 2, 3] }""";
        PatchRecipe.Parse(Encoding.UTF8.GetBytes(Obj(fine)));

        var bad = new Dictionary<string, string>
        {
            ["not JSON"] = "{",
            ["not an object"] = "[]",
            ["no format"] = """{ "levels": {} }""",
            ["newer format"] = """{ "format": 2, "levels": {} }""",
            ["format as text"] = """{ "format": "1", "levels": {} }""",
            ["no levels"] = """{ "format": 1 }""",
            ["empty levels"] = """{ "format": 1, "levels": {} }""",
            ["unknown top setting"] = """{ "format": 1, "run": "x", "levels": {} }""",
            ["level with a path"] = $$"""{ "format": 1, "levels": { "../h002": { "base_sha256": "{{sha}}", "objects": [ {{fine}} ] } } }""",
            ["level twice by case"] = $$"""{ "format": 1, "levels": { "h002": { "base_sha256": "{{sha}}", "objects": [ {{fine}} ] }, "H002": { "base_sha256": "{{sha}}", "objects": [ {{fine}} ] } } }""",
            ["short hash"] = Wrap("""{ "base_sha256": "ABC", "objects": [] }"""),
            ["nothing to add"] = Wrap($$"""{ "base_sha256": "{{sha}}" }"""),
            ["unknown level setting"] = Wrap($$"""{ "base_sha256": "{{sha}}", "remove": [] }"""),
            ["replaced setting listed twice"] = Wrap($$"""{ "base_sha256": "{{sha}}", "add": [], "add": [] }"""),
            ["asset of another type"] = Add("""{ "type": "SIMP", "name": "x", "file": "a" }"""),
            ["asset with file and from"] = Add($$"""{ "type": "MODL", "name": "x", "file": "a", "from": { "level": "l011", "id": "50721989", "sha256": "{{sha}}" } }"""),
            ["asset with neither"] = Add("""{ "type": "MODL", "name": "x" }"""),
            ["texture name without .RW3"] = Add("""{ "type": "RWTX", "name": "x", "file": "a" }"""),
            ["name not ASCII"] = Add("""{ "type": "MODL", "name": "é", "file": "a" }"""),
            ["name too long"] = Add($$"""{ "type": "MODL", "name": "{{new string('x', 64)}}", "file": "a" }"""),
            ["path going up"] = Add("""{ "type": "MODL", "name": "x", "file": "../a" }"""),
            ["path going up later"] = Add("""{ "type": "MODL", "name": "x", "file": "a/../../b" }"""),
            ["rooted path"] = Add("""{ "type": "MODL", "name": "x", "file": "/a" }"""),
            ["drive path"] = Add("""{ "type": "MODL", "name": "x", "file": "C:/a" }"""),
            ["backslash path"] = Add("""{ "type": "MODL", "name": "x", "file": "..\\a" }"""),
            ["stream path"] = Add("""{ "type": "MODL", "name": "x", "file": "a:b" }"""),
            ["device path"] = Add("""{ "type": "MODL", "name": "x", "file": "NUL.modl" }"""),
            ["from a bad level"] = Add($$"""{ "type": "MODL", "from": { "level": "../l011", "id": "50721989", "sha256": "{{sha}}" } }"""),
            ["from a bad ID"] = Add($$"""{ "type": "MODL", "from": { "level": "l011", "id": "xyz", "sha256": "{{sha}}" } }"""),
            ["from a bad hash"] = Add("""{ "type": "MODL", "from": { "level": "l011", "id": "50721989", "sha256": "1" } }"""),
            ["object without pos"] = Obj("""{ "name": "a", "model": "m" }"""),
            ["pos of two"] = Obj("""{ "name": "a", "model": "m", "pos": [1, 2] }"""),
            ["pos as text"] = Obj("""{ "name": "a", "model": "m", "pos": ["1", 2, 3] }"""),
            ["pos out of range"] = Obj("""{ "name": "a", "model": "m", "pos": [1e30, 2, 3] }"""),
            ["huge number"] = Obj("""{ "name": "a", "model": "m", "pos": [1e999, 2, 3] }"""),
            ["zero scale"] = Obj("""{ "name": "a", "model": "m", "pos": [1, 2, 3], "scale": [0, 1, 1] }"""),
            ["negative tint"] = Obj("""{ "name": "a", "model": "m", "pos": [1, 2, 3], "tint": [-1, 1, 1] }"""),
            ["see through above 1"] = Obj("""{ "name": "a", "model": "m", "pos": [1, 2, 3], "see_through": 2 }"""),
            ["solid as a number"] = Obj("""{ "name": "a", "model": "m", "pos": [1, 2, 3], "solid": 1 }"""),
            ["unknown object setting"] = Obj("""{ "name": "a", "model": "m", "pos": [1, 2, 3], "script": "x" }"""),
            ["too many objects"] = Obj(string.Join(",", Enumerable.Repeat(fine, PatchRecipe.MaxObjectsPerLevel + 1))),
        };
        foreach (var (name, json) in bad)
        {
            bool refused = false;
            try { PatchRecipe.Parse(Encoding.UTF8.GetBytes(json)); }
            catch (PatchException) { refused = true; }
            Check("recipe refused: " + name, refused);
        }
        bool tooBig = false;
        try { PatchRecipe.Parse(new byte[PatchRecipe.MaxRecipeBytes + 1]); }
        catch (PatchException) { tooBig = true; }
        Check("recipe refused: too large", tooBig);

        // Own assets: the structure check.
        foreach (var (name, bytes, model) in new (string, byte[], bool)[]
        {
            ("empty model", Array.Empty<byte>(), true),
            ("texture as a model", Fixtures.Texture("x"), true),
            ("model cut short", Fixtures.Model("x")[..40], true),
            ("model of too many triangles", Fixtures.Model("x", RwCheck.MaxTriangles + 1), true),
            // The file can say one thing in its header and another in its mesh list: both are read from the bytes.
            ("header says little, one material's vertices wrap the game's count", Fixtures.Model("x", 12, new uint[] { RwCheck.MaxMaterialVertices + 1 }), true),
            ("header says little, the materials add up to too many triangles", Fixtures.Model("x", 12, new uint[] { 30000, 30000 }), true),
            ("header says little, a strip's vertices wrap the game's count", Fixtures.Model("x", 12, new uint[] { RwCheck.MaxMaterialVertices + 1 }, strip: true), true),
            ("header says too many, the mesh list says little", Fixtures.Model("x", RwCheck.MaxTriangles + 1, new uint[] { 36 }), true),
            ("no mesh list", Fixtures.Model("x", noMeshList: true), true),
            ("mesh list cut short", ShortMeshList(), true),
            ("model with a section size past the end", BadSize(Fixtures.Model("x")), true),
            ("empty texture", Array.Empty<byte>(), false),
            ("model as a texture", Fixtures.Model("x"), false),
            ("texture cut short", Fixtures.Texture("x")[..80], false),
            ("texture of a bad size", Fixtures.Texture("x", 4096, 4), false),
        })
        {
            bool refused = false;
            try { if (model) RwCheck.Model(bytes, name); else RwCheck.Texture(bytes, name); }
            catch (PatchException) { refused = true; }
            Check("asset refused: " + name, refused);
        }
        RwCheck.Model(Fixtures.Model("x", RwCheck.MaxTriangles), "at the limit");
        RwCheck.Model(Fixtures.Model("x", RwCheck.MaxTriangles, new uint[] { 3000, RwCheck.MaxTriangles * 3 - 3000 }), "at the limit, two materials");
        RwCheck.Model(Fixtures.Model("x", 12, new uint[] { 19538, 2000 }, strip: true), "strips as long as the disc's: their index counts aren't triangles");
        RwCheck.Model(Fixtures.Model("x", 12, new uint[] { RwCheck.MaxMaterialVertices }, strip: true), "a strip at the vertex limit");
        var trailing = Fixtures.Model("x");
        BinaryPrimitives.WriteUInt32LittleEndian(trailing.AsSpan(4), BinaryPrimitives.ReadUInt32LittleEndian(trailing.AsSpan(4)) + 28);
        var withAtomic = trailing[..^12].Concat(new byte[] { 0x14, 0, 0, 0, 40, 0, 0, 0, 0x10, 3, 0, 0 }).Concat(new byte[16]).Concat(trailing[^12..]).ToArray();
        // Disc skinned models: the atomic after the geometry list claims 12 bytes more than the clump has.
        RwCheck.Model(withAtomic, "an atomic after the geometry list running past the clump, as the disc's skinned models have");
        RwCheck.Model(Fixtures.Model("x").Concat(new byte[] { 0x9E, 0xD1, 0x33, 0xBF, 0xFF, 0xFF, 0xFF, 0x3F, 4, 0, 0xF7, 0x18 }).ToArray(), "stray bytes after the clump, as the disc's skinned models have");
        RwCheck.Texture(Fixtures.Texture("x"), "fine");

        // Through the whole build: the launcher reads the mod's model file, and the recipe has no say in it.
        using (var s = new Sandbox())
        {
            s.Mod("wrap", null, Fixtures.Model("wrap", 12, new uint[] { RwCheck.MaxMaterialVertices + 1 }));
            s.Mod("many", null, Fixtures.Model("many", RwCheck.MaxTriangles + 1));
            s.Mod("fine");
            var r = s.Ensure();
            Check("cap: a material over the limit is refused, with the reason", r.Mods["wrap"].State == PatchState.Problem && r.Mods["wrap"].Text.Contains("vertices"), r.Mods["wrap"].Text);
            Check("cap: a model over the limit is refused, with the reason", r.Mods["many"].State == PatchState.Problem && r.Mods["many"].Text.Contains("triangles"), r.Mods["many"].Text);
            Check("cap: another mod is still built", r.Mods["fine"].State == PatchState.Ready, r.Mods["fine"].Text);
            var hip = new HipFile(File.ReadAllBytes(s.OutputH002));
            Check("cap: the refused models aren't in the level", hip.Find(HipFile.HashName("wrap")) == null && hip.Find(HipFile.HashName("many")) == null);
        }
        using (var s = new Sandbox())
        {
            var claims = s.Crate("liar").Replace("\"name\": \"liar\", \"file\": \"assets/m.modl\"", "\"name\": \"liar\", \"file\": \"assets/m.modl\", \"triangles\": 12");
            Check("cap: the test recipe does state a count", claims.Contains("\"triangles\": 12"));
            s.Mod("liar", claims, Fixtures.Model("liar"));
            var r = s.Ensure();
            Check("cap: a recipe can't state a triangle count", r.Mods["liar"].State == PatchState.Problem && r.Mods["liar"].Text.Contains("triangles"), r.Mods["liar"].Text);
        }
    }

    static byte[] ShortMeshList()
    {
        var list = Fixtures.MeshList(new uint[] { 36 });
        BinaryPrimitives.WriteUInt32LittleEndian(list.AsSpan(12), 1000);    // claims more indices than the section holds
        var d = Fixtures.Model("x", 12, new uint[] { 36 });
        int at = d.AsSpan().IndexOf(Fixtures.MeshList(new uint[] { 36 }));
        list.CopyTo(d, at);
        return d;
    }

    static byte[] BadSize(byte[] d)
    {
        var copy = (byte[])d.Clone();
        BinaryPrimitives.WriteUInt32LittleEndian(copy.AsSpan(4), 0x7FFFFFFF);
        return copy;
    }

    static void Builds()
    {
        using (var s = new Sandbox())
        {
            var noMods = s.Ensure();
            Check("no mods: nothing built", !Directory.Exists(s.Output) && noMods.Error == null && !noMods.Rebuilt);

            s.Mod("crate");
            var first = s.Ensure();
            Check("builds", first.Rebuilt && first.Error == null, first.Error ?? "");
            Check("status ready", first.Mods["crate"].State == PatchState.Ready, first.Mods["crate"].Text);
            Check("level is at the disc's path", File.Exists(s.OutputH002));
            var hip = new HipFile(File.ReadAllBytes(s.OutputH002));
            var obj = hip.Find(HipFile.HashName("crate OBJ"));
            Check("object added", obj is { TypeName: "SIMP" });
            Check("model added under its name's hash", hip.Find(HipFile.HashName("crate")) is { TypeName: "MODL" });
            Check("texture added under its name's hash", hip.Find(HipFile.HashName("crate.RW3")) is { TypeName: "RWTX" });
            var o = obj!.Data.Span;
            Check("object's ID field", BinaryPrimitives.ReadUInt32BigEndian(o) == HipFile.HashName("crate OBJ"));
            Check("object's yaw is radians", Math.Abs(BinaryPrimitives.ReadSingleBigEndian(o[0x10..]) - MathF.PI / 2) < 1e-6);
            Check("object's pos", BinaryPrimitives.ReadSingleBigEndian(o[0x1C..]) == 1 && BinaryPrimitives.ReadSingleBigEndian(o[0x20..]) == 2 && BinaryPrimitives.ReadSingleBigEndian(o[0x24..]) == 3);
            Check("object's scale defaults to 1", BinaryPrimitives.ReadSingleBigEndian(o[0x28..]) == 1);
            Check("object's fade defaults to off", BinaryPrimitives.ReadSingleBigEndian(o[0x40..]) == 1);
            Check("object's model", BinaryPrimitives.ReadUInt32BigEndian(o[0x48..]) == HipFile.HashName("crate"));
            Check("object is walk-through by default", o[0x58] == 0);
            Check("the disc is untouched", Fixtures.Sha(File.ReadAllBytes(Path.Combine(s.Disc, "files", "hb", "h002.HIP"))) == Fixtures.Sha(Fixtures.Level(1)));

            var mtime = File.GetLastWriteTimeUtc(s.OutputH002);
            var again = s.Ensure();
            Check("nothing changed: no rebuild", !again.Rebuilt && File.GetLastWriteTimeUtc(s.OutputH002) == mtime);
            Check("nothing changed: still ready", again.Mods["crate"].State == PatchState.Ready);

            File.WriteAllBytes(Path.Combine(s.Mods, "crate", "assets", "m.modl"), Fixtures.Model("changed"));
            Check("an asset changed: rebuild", s.Ensure().Rebuilt);
            File.WriteAllText(Path.Combine(s.Mods, "crate", PatchRecipe.FileName), s.Crate("crate").Replace("[1, 2, 3]", "[4, 5, 6]"));
            Check("the recipe changed: rebuild", s.Ensure().Rebuilt);

            File.Delete(s.OutputH002);
            Check("a built level went missing: rebuild", s.Ensure().Rebuilt && File.Exists(s.OutputH002));

            File.WriteAllText(Path.Combine(s.Mods, "crate", "disabled"), "");
            var off = s.Ensure();
            Check("ticked off: output removed", off.Rebuilt && !Directory.Exists(s.Output));
            Check("ticked off: status off", off.Mods["crate"].State == PatchState.Off);
            Check("ticked off: nothing to say", off.Mods["crate"].Text.Length == 0);
            File.Delete(Path.Combine(s.Mods, "crate", "disabled"));
            Check("ticked on: built again", s.Ensure().Rebuilt && File.Exists(s.OutputH002));
            Directory.Delete(Path.Combine(s.Mods, "crate"), true);
            Check("mod deleted: output removed", s.Ensure().Rebuilt && !Directory.Exists(s.Output));
        }

        using (var s = new Sandbox())  // a mod from another level's assets
        {
            var source = new HipFile(File.ReadAllBytes(Path.Combine(s.Disc, "files", "lb", "l011.HIP")));
            uint modelId = HipFile.HashName("barrel"), textureId = HipFile.HashName("keg.RW3");
            var modelSha = Fixtures.Sha(source.Find(modelId)!.Data.ToArray());
            var textureSha = Fixtures.Sha(source.Find(textureId)!.Data.ToArray());
            string Recipe(string msha) => $$"""
                { "format": 1, "levels": { "h002": { "base_sha256": "{{s.H002Sha}}",
                  "add": [ { "type": "RWTX", "from": { "level": "l011", "id": "{{textureId:X8}}", "sha256": "{{textureSha}}" } },
                           { "type": "MODL", "from": { "level": "L011", "id": "{{modelId:X8}}", "sha256": "{{msha}}" } } ],
                  "objects": [ { "name": "PROP", "model": "barrel", "pos": [0, 0, 0], "scale": [0.5, 0.5, 0.5], "tint": [0.25, 0.5, 0.75], "solid": true, "see_through": 0.5 } ] } } }
                """;
            s.Mod("prop", Recipe(modelSha));
            var r = s.Ensure();
            Check("from another level: ready", r.Mods["prop"].State == PatchState.Ready, r.Mods["prop"].Text);
            var hip = new HipFile(File.ReadAllBytes(s.OutputH002));
            Check("from another level: same bytes", hip.Find(modelId)?.Data.Span.SequenceEqual(source.Find(modelId)!.Data.Span) == true);
            Check("from another level: keeps its name", hip.Find(modelId)?.Name == "barrel");
            var o = hip.Find(HipFile.HashName("PROP"))!.Data.Span;
            Check("object's model is the copied one", BinaryPrimitives.ReadUInt32BigEndian(o[0x48..]) == modelId);
            Check("object's scale", BinaryPrimitives.ReadSingleBigEndian(o[0x28..]) == 0.5f);
            Check("object's tint", BinaryPrimitives.ReadSingleBigEndian(o[0x38..]) == 0.5f);
            Check("object's fade", BinaryPrimitives.ReadSingleBigEndian(o[0x40..]) == 0.5f);
            Check("object is solid", o[0x58] == 2);

            File.WriteAllText(Path.Combine(s.Mods, "prop", PatchRecipe.FileName), Recipe(new string('0', 64)));
            var wrong = s.Ensure();
            Check("from another level, wrong hash: skipped", wrong.Mods["prop"].State == PatchState.Problem && !Directory.Exists(s.Output));
            File.WriteAllText(Path.Combine(s.Mods, "prop", PatchRecipe.FileName), Recipe(modelSha).Replace(modelId.ToString("X8"), "12345678"));
            Check("from another level, no such asset: skipped", s.Ensure().Mods["prop"].State == PatchState.Problem);
            File.WriteAllText(Path.Combine(s.Mods, "prop", PatchRecipe.FileName), Recipe(modelSha).Replace("\"type\": \"MODL\"", "\"type\": \"RWTX\""));
            Check("from another level, wrong type: skipped", s.Ensure().Mods["prop"].State == PatchState.Problem);
        }

        using (var s = new Sandbox())  // two mods on one level, in name order
        {
            s.Mod("b-mod");
            s.Mod("a-mod");
            var r = s.Ensure();
            Check("two mods: both ready", r.Mods["a-mod"].State == PatchState.Ready && r.Mods["b-mod"].State == PatchState.Ready);
            var hip = new HipFile(File.ReadAllBytes(s.OutputH002));
            Check("two mods: both in the level", hip.Find(HipFile.HashName("a-mod OBJ")) != null && hip.Find(HipFile.HashName("b-mod OBJ")) != null);
            Check("two mods: name order", hip.Find(HipFile.HashName("a-mod.RW3"))!.Offset < hip.Find(HipFile.HashName("b-mod.RW3"))!.Offset);
        }

        using (var s = new Sandbox())  // same ID, same bytes: shared
        {
            var model = Fixtures.Model("shared");
            string Recipe(string obj) => $$"""
                { "format": 1, "levels": { "h002": { "base_sha256": "{{s.H002Sha}}", "add": [ { "type": "MODL", "name": "shared", "file": "assets/m.modl" } ],
                  "objects": [ { "name": "{{obj}}", "model": "shared", "pos": [1, 2, 3] } ] } } }
                """;
            s.Mod("a", Recipe("A OBJ"), model);
            s.Mod("b", Recipe("B OBJ"), model);
            var r = s.Ensure();
            Check("same ID and bytes: both ready", r.Mods["a"].State == PatchState.Ready && r.Mods["b"].State == PatchState.Ready, r.Mods["b"].Text);
            var hip = new HipFile(File.ReadAllBytes(s.OutputH002));
            Check("same ID and bytes: one copy", hip.Assets.Count(x => x.Id == HipFile.HashName("shared")) == 1);
            Check("same ID and bytes: both objects", hip.Find(HipFile.HashName("A OBJ")) != null && hip.Find(HipFile.HashName("B OBJ")) != null);
        }

        using (var s = new Sandbox())  // same ID, different bytes: the later mod skipped and named
        {
            string Recipe(string obj) => $$"""
                { "format": 1, "levels": { "h002": { "base_sha256": "{{s.H002Sha}}", "add": [ { "type": "MODL", "name": "shared", "file": "assets/m.modl" } ],
                  "objects": [ { "name": "{{obj}}", "model": "shared", "pos": [1, 2, 3] } ] } } }
                """;
            s.Mod("first", Recipe("FIRST"), Fixtures.Model("one"));
            s.Mod("second", Recipe("SECOND"), Fixtures.Model("two"));
            var r = s.Ensure();
            Check("clash: first ready", r.Mods["first"].State == PatchState.Ready);
            Check("clash: second skipped", r.Mods["second"].State == PatchState.Problem);
            Check("clash: names the other mod", r.Mods["second"].Text.Contains("first"), r.Mods["second"].Text);
            var hip = new HipFile(File.ReadAllBytes(s.OutputH002));
            Check("clash: all of the second left out", hip.Find(HipFile.HashName("SECOND")) == null);
            Check("clash: first's bytes kept", hip.Find(HipFile.HashName("shared"))?.Data.Span.SequenceEqual(Fixtures.Model("one")) == true);
            var cached = s.Ensure();
            Check("clash: reported again with no rebuild", !cached.Rebuilt && cached.Mods["second"].State == PatchState.Problem && cached.Mods["second"].Text.Contains("first"));
        }

        using (var s = new Sandbox())  // clash with what the level already has
        {
            s.Mod("clash", s.Crate("tree"), Fixtures.Model("not the level's"));
            var r = s.Ensure();
            Check("clash with the level: skipped", r.Mods["clash"].State == PatchState.Problem && !Directory.Exists(s.Output), r.Mods["clash"].Text);
        }

        using (var s = new Sandbox())  // one good, one bad, one wrong disc
        {
            s.Mod("good");
            s.Mod("wrong-disc", s.Crate("w", sha: new string('0', 64)));
            s.Mod("no-level", s.Crate("n", "x999"));
            s.Mod("broken").ToString();
            File.WriteAllText(Path.Combine(s.Mods, "broken", PatchRecipe.FileName), "{");
            s.Mod("missing-asset");
            File.Delete(Path.Combine(s.Mods, "missing-asset", "assets", "t.rwtx"));
            s.Mod("no-model", $$"""{ "format": 1, "levels": { "h002": { "base_sha256": "{{s.H002Sha}}", "objects": [ { "name": "O", "model": "nope", "pos": [1, 2, 3] } ] } } }""");
            s.Mod("bad-bytes", null, Encoding.ASCII.GetBytes("not a model"));
            var r = s.Ensure();
            Check("others still apply", r.Mods["good"].State == PatchState.Ready && File.Exists(s.OutputH002));
            foreach (var bad in new[] { "wrong-disc", "no-level", "broken", "missing-asset", "no-model", "bad-bytes" })
                Check($"{bad}: problem with a reason", r.Mods[bad].State == PatchState.Problem && r.Mods[bad].Text.Length > 0);
            Check("wrong disc says so", r.Mods["wrong-disc"].Text.Contains("different copy"), r.Mods["wrong-disc"].Text);
            var hip = new HipFile(File.ReadAllBytes(s.OutputH002));
            Check("only the good mod is in the level", hip.Find(HipFile.HashName("good OBJ")) != null && hip.Find(HipFile.HashName("w OBJ")) == null && hip.Find(HipFile.HashName("bad-bytes OBJ")) == null);
        }

        using (var s = new Sandbox())  // all or nothing across levels
        {
            var recipe = $$"""
                { "format": 1, "levels": {
                  "h002": { "base_sha256": "{{s.H002Sha}}", "add": [ { "type": "MODL", "name": "m", "file": "assets/m.modl" } ], "objects": [ { "name": "A", "model": "m", "pos": [1, 2, 3] } ] },
                  "l011": { "base_sha256": "{{s.L011Sha}}", "objects": [ { "name": "B", "model": "nothing", "pos": [1, 2, 3] } ] } } }
                """;
            s.Mod("half", recipe);
            var r = s.Ensure();
            Check("a mod fails in one level: nothing applied", r.Mods["half"].State == PatchState.Problem && !Directory.Exists(s.Output));
        }

        using (var s = new Sandbox())  // budget
        {
            var big = Fixtures.Texture("big", 8, 8, 600 * 1024);
            string Recipe(string name) => $$"""
                { "format": 1, "levels": { "h002": { "base_sha256": "{{s.H002Sha}}", "add": [ { "type": "RWTX", "name": "{{name}}.RW3", "file": "assets/t.rwtx" } ] } } }
                """;
            s.Mod("a", Recipe("a"), null, big);
            s.Mod("b", Recipe("b"), null, big);
            var r = s.Ensure();
            Check("budget: first fits", r.Mods["a"].State == PatchState.Ready, r.Mods["a"].Text);
            Check("budget: second doesn't", r.Mods["b"].State == PatchState.Problem && r.Mods["b"].Text.Contains("too big"), r.Mods["b"].Text);
            var hip = new HipFile(File.ReadAllBytes(s.OutputH002));
            Check("budget: second left out", hip.Find(HipFile.HashName("b.RW3")) == null);
        }

        using (var s = new Sandbox())  // a whole-level mod beats a patch: warned
        {
            s.Mod("patch");
            var replacing = Path.Combine(s.Mods, "whole", "files", "hb");
            Directory.CreateDirectory(replacing);
            File.WriteAllBytes(Path.Combine(replacing, "h002.HIP"), Fixtures.Level(9));
            var r = s.Ensure();
            Check("override: still ready", r.Mods["patch"].State == PatchState.Ready);
            Check("override: warned", r.Mods["patch"].Text.Contains("whole"), r.Mods["patch"].Text);
            Check("override: not a patch mod itself", !r.Mods.ContainsKey("whole"));
            File.WriteAllText(Path.Combine(s.Mods, "whole", "disabled"), "");
            Check("override off: no warning", !s.Ensure().Mods["patch"].Text.Contains("whole"));
        }

        using (var s = new Sandbox())  // a folder holding both is fine
        {
            var dir = s.Mod("both");
            Directory.CreateDirectory(Path.Combine(dir, "files", "other"));
            File.WriteAllText(Path.Combine(dir, "files", "other", "x.txt"), "x");
            Check("a mod with files and a patch", s.Ensure().Mods["both"].State == PatchState.Ready);
        }

        using (var s = new Sandbox())  // links
        {
            var dir = s.Mod("linked");
            var outside = Path.Combine(s.Root, "secret.modl");
            File.WriteAllBytes(outside, Fixtures.Model("secret"));
            var link = Path.Combine(dir, "assets", "m.modl");
            File.Delete(link);
            try
            {
                File.CreateSymbolicLink(link, outside);
                var r = s.Ensure();
                Check("a link in a mod: refused", r.Mods["linked"].State == PatchState.Problem && r.Mods["linked"].Text.Contains("link"), r.Mods["linked"].Text);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Console.WriteLine("skipped: can't make a link here (" + e.GetType().Name + ")"); }
        }
    }

    static void DiscFaults()
    {
        using (var s = new Sandbox())  // no disc at all
        {
            s.Mod("crate", s.Crate("crate"));
            Directory.Delete(Path.Combine(s.Disc, "files"), true);
            var r = s.Ensure();
            Check("no disc: mod problem, no throw", r.Mods["crate"].State == PatchState.Problem && !Directory.Exists(s.Output));
        }
        using (var s = new Sandbox())  // a corrupt level that still has the hash the mod asks for
        {
            var path = Path.Combine(s.Disc, "files", "hb", "h002.HIP");
            File.WriteAllBytes(path, new byte[128]);
            s.Mod("crate", s.Crate("crate"));
            var r = s.Ensure();
            Check("unreadable level: mod problem, no throw", r.Error == null && r.Mods["crate"].State == PatchState.Problem && !Directory.Exists(s.Output), r.Error ?? r.Mods["crate"].Text);
        }
        using (var s = new Sandbox())  // the disc changed since the build
        {
            s.Mod("crate", s.Crate("crate"));
            s.Ensure();
            File.WriteAllBytes(Path.Combine(s.Disc, "files", "hb", "h002.HIP"), Fixtures.Level(5));
            var r = s.Ensure();
            Check("disc changed: skipped and output removed", r.Mods["crate"].State == PatchState.Problem && !Directory.Exists(s.Output));
        }
        using (var s = new Sandbox())  // a level with no room
        {
            File.WriteAllBytes(Path.Combine(s.Disc, "files", "hb", "h002.HIP"), Fixtures.Level(1, withSimple: false));
            s.Mod("crate", s.Crate("crate"));
            var r = s.Ensure();
            Check("no simple object to copy: problem", r.Mods["crate"].State == PatchState.Problem && r.Mods["crate"].Text.Contains("simple"), r.Mods["crate"].Text);
        }
    }

    static void Output()
    {
        using (var s = new Sandbox())  // a folder that isn't ours
        {
            Directory.CreateDirectory(s.Output);
            File.WriteAllText(Path.Combine(s.Output, "mine.txt"), "player's");
            s.Mod("crate");
            var r = s.Ensure();
            Check("foreign folder: kept", File.ReadAllText(Path.Combine(s.Output, "mine.txt")) == "player's");
            Check("foreign folder: said so", r.Mods["crate"].State == PatchState.Problem && r.Mods["crate"].Text.Contains(PatchBuilder.OutputFolder), r.Mods["crate"].Text);
            Check("foreign folder: not built over", !File.Exists(s.OutputH002));
        }
        using (var s = new Sandbox())  // a folder that isn't ours but has our file's name
        {
            Directory.CreateDirectory(s.Output);
            File.WriteAllText(Path.Combine(s.Output, "build.json"), "not ours");
            s.Mod("crate");
            s.Ensure();
            Check("foreign build.json: kept", File.ReadAllText(Path.Combine(s.Output, "build.json")) == "not ours");
        }
        using (var s = new Sandbox())  // the game is running
        {
            s.Mod("crate");
            var r = s.Ensure(() => true);
            Check("game running: nothing written", !Directory.Exists(s.Output) && !r.Rebuilt);
            Check("game running: said so", r.Mods["crate"].Text.Contains("Close the game"), r.Mods["crate"].Text);
            s.Ensure();
            var before = s.Snapshot();
            File.WriteAllText(Path.Combine(s.Mods, "crate", "disabled"), "");
            s.Ensure(() => true);
            File.Delete(Path.Combine(s.Mods, "crate", "disabled"));
            Check("game running: a built level stays", s.Snapshot() == before);
        }
        using (var s = new Sandbox())  // a build killed part way leaves staging; the next one ignores it, and the game never sees it
        {
            s.Mod("crate");
            s.Ensure();
            var good = Fixtures.Sha(File.ReadAllBytes(s.OutputH002));
            var staged = Path.Combine(s.Cache, "mod-build", PatchBuilder.OutputFolder, "files", "hb");
            Directory.CreateDirectory(staged);
            File.WriteAllBytes(Path.Combine(staged, "h002.HIP"), new byte[10]);
            Check("killed build: staging isn't under mods", !Path.GetFullPath(staged).StartsWith(Path.GetFullPath(s.Mods), StringComparison.Ordinal));
            s.Ensure();
            Check("killed build: the last good level stays", Fixtures.Sha(File.ReadAllBytes(s.OutputH002)) == good);
            File.WriteAllText(Path.Combine(s.Mods, "crate", PatchRecipe.FileName), s.Crate("crate").Replace("[1, 2, 3]", "[7, 8, 9]"));
            Directory.CreateDirectory(staged);
            File.WriteAllBytes(Path.Combine(staged, "h002.HIP"), new byte[10]);
            s.Ensure();
            Check("killed build: rebuilt whole", new HipFile(File.ReadAllBytes(s.OutputH002)).Find(HipFile.HashName("crate OBJ")) != null);
            Check("staging cleaned up", !Directory.Exists(Path.Combine(s.Cache, "mod-build")));
        }
        using (var s = new Sandbox())  // deleted between the delete and the move: no folder, game unmodded, next Ensure builds it
        {
            s.Mod("crate");
            s.Ensure();
            Directory.Delete(s.Output, true);
            Check("no output: rebuilt", s.Ensure().Rebuilt && File.Exists(s.OutputH002));
        }
        using (var s = new Sandbox())  // a stamp that's unreadable counts as foreign, and is reported, not a crash
        {
            s.Mod("crate");
            s.Ensure();
            File.WriteAllText(Path.Combine(s.Output, "build.json"), "{ broken");
            var r = s.Ensure();
            Check("broken stamp: reported, no throw", r.Error == null && r.Mods["crate"].State == PatchState.Problem);
        }
    }

    delegate void Fill(ZipArchive zip);

    static string MakeZip(Sandbox s, string name, Fill fill)
    {
        var dir = Path.Combine(s.Root, "zips", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        var path = Path.Combine(dir, name);
        using var zip = ZipFile.Open(path, ZipArchiveMode.Create);
        fill(zip);
        return path;
    }

    static void Entry(ZipArchive zip, string name, byte[] data, int? attributes = null)
    {
        var e = zip.CreateEntry(name);
        if (attributes != null) e.ExternalAttributes = attributes.Value;
        using var w = e.Open();
        w.Write(data);
    }

    static void Entry(ZipArchive zip, string name, string text) => Entry(zip, name, Encoding.UTF8.GetBytes(text));

    static void Zips()
    {
        var marker = Encoding.UTF8.GetBytes("x");
        var attacks = new Dictionary<string, Fill>
        {
            ["going up"] = z => { Entry(z, "files/x", "x"); Entry(z, "../evil.txt", "x"); },
            ["going up in the middle"] = z => { Entry(z, "files/x", "x"); Entry(z, "a/../../evil.txt", "x"); },
            ["backslash going up"] = z => { Entry(z, "files/x", "x"); Entry(z, "..\\evil.txt", "x"); },
            ["rooted"] = z => { Entry(z, "files/x", "x"); Entry(z, "/evil.txt", "x"); },
            ["backslash rooted"] = z => { Entry(z, "files/x", "x"); Entry(z, "\\evil.txt", "x"); },
            ["drive"] = z => { Entry(z, "files/x", "x"); Entry(z, "C:/evil.txt", "x"); },
            ["drive relative"] = z => { Entry(z, "files/x", "x"); Entry(z, "C:evil.txt", "x"); },
            ["stream"] = z => { Entry(z, "files/x", "x"); Entry(z, "files/x:evil", "x"); },
            ["trailing dot"] = z => { Entry(z, "files/x", "x"); Entry(z, "files/evil.", "x"); },
            ["trailing space"] = z => { Entry(z, "files/x", "x"); Entry(z, "files/evil ", "x"); },
            ["reserved name"] = z => { Entry(z, "files/x", "x"); Entry(z, "files/COM1.txt", "x"); },
            ["control character"] = z => { Entry(z, "files/x", "x"); Entry(z, "files/a\u0001b", "x"); },
            ["unix link"] = z => { Entry(z, "files/x", "x"); Entry(z, "files/link", Encoding.ASCII.GetBytes("/etc/passwd"), unchecked((int)0xA1FF0000)); },
            ["device"] = z => { Entry(z, "files/x", "x"); Entry(z, "files/dev", marker, unchecked((int)0x21B60000)); },
            ["windows link"] = z => { Entry(z, "files/x", "x"); Entry(z, "files/link", marker, (int)FileAttributes.ReparsePoint); },
            ["same name twice"] = z => { Entry(z, "files/x", "x"); Entry(z, "files/x", "y"); },
            ["same name by case"] = z => { Entry(z, "files/x", "x"); Entry(z, "FILES/X", "y"); },
            ["a file and a folder of one name"] = z => { Entry(z, "files/x", "x"); Entry(z, "files/x/y", "y"); },
            ["not a mod"] = z => Entry(z, "readme.txt", "x"),
            ["empty"] = _ => { },
            ["bad recipe"] = z => Entry(z, PatchRecipe.FileName, "{"),
            ["recipe with a missing asset"] = z => Entry(z, PatchRecipe.FileName, Recipe("missing/a.modl")),
            ["recipe with a bad asset"] = z => { Entry(z, PatchRecipe.FileName, Recipe("a.modl")); Entry(z, "a.modl", "not a model"); },
            ["recipe going up"] = z => Entry(z, PatchRecipe.FileName, Recipe("../a.modl")),
            ["bomb"] = z => Entry(z, "files/x", new byte[64 << 20]),
            ["wrapped and named for the output"] = z => Entry(z, "!patched/files/x", "x"),
            ["wrapped in a dots folder"] = z => Entry(z, "../files/x", "x"),
        };
        foreach (var (name, fill) in attacks)
        {
            using var s = new Sandbox();
            s.Mod("existing");
            s.Ensure();
            var before = s.Snapshot();
            var outsideBefore = Directory.GetFileSystemEntries(s.Root).Length;
            var zip = MakeZip(s, "evil.zip", fill);
            bool refused = false;
            try { ModZip.Prepare(zip, s.Cache); }
            catch (PatchException) { refused = true; }
            Check("zip refused: " + name, refused);
            Check("zip left the mods folder: " + name, s.Snapshot() == before);
            Check("zip wrote nothing outside: " + name, Directory.GetFileSystemEntries(s.Root).Length == outsideBefore + 1 && !File.Exists(Path.Combine(s.Root, "UserData", "evil.txt")) && !File.Exists(Path.Combine(s.Root, "evil.txt")));
            Check("zip left no staging: " + name, !Directory.Exists(Path.Combine(s.Cache, "mod-install")) || Directory.GetDirectories(Path.Combine(s.Cache, "mod-install")).Length == 0);
        }

        using (var s = new Sandbox())  // a file that isn't a zip
        {
            var path = Path.Combine(s.Root, "fake.zip");
            File.WriteAllText(path, "not a zip");
            bool refused = false;
            try { ModZip.Prepare(path, s.Cache); }
            catch (PatchException) { refused = true; }
            Check("zip refused: not a zip", refused);
        }

        using (var s = new Sandbox())  // a zip whose headers call a big file small: what comes out must not exceed the claim, or it's refused
        {
            var zip = MakeZip(s, "liar.zip", z => Entry(z, "files/x", new byte[1 << 20]));
            Lie(zip);
            try
            {
                var mod = ModZip.Prepare(zip, s.Cache);
                Check("lying size: no more than claimed", new FileInfo(Path.Combine(mod.Dir, "files", "x")).Length <= 1);
                ModZip.Discard(mod);
            }
            catch (PatchException) { passed++; }
            Check("lying size: no staging", !Directory.Exists(Path.Combine(s.Cache, "mod-install")) || Directory.GetDirectories(Path.Combine(s.Cache, "mod-install")).Length == 0);
        }

        using (var s = new Sandbox())  // good ones
        {
            var flat = MakeZip(s, "flat-mod.zip", z => { Entry(z, "files/hb/x.txt", "x"); Entry(z, "readme.txt", "r"); });
            var a = ModZip.Prepare(flat, s.Cache);
            Check("flat zip: named after the zip", a.Name == "flat-mod");
            Check("flat zip: not in mods yet", !Directory.Exists(Path.Combine(s.Mods, "flat-mod")));
            ModZip.Commit(a, s.Mods);
            Check("flat zip: installed", File.Exists(Path.Combine(s.Mods, "flat-mod", "files", "hb", "x.txt")) && File.Exists(Path.Combine(s.Mods, "flat-mod", "readme.txt")));
            Check("flat zip: staging gone", !Directory.Exists(a.Staging));

            var wrapped = MakeZip(s, "download-1.0.zip", z => { Entry(z, "My Mod/", Array.Empty<byte>()); Entry(z, "My Mod/textures/a.png", "x"); });
            var b = ModZip.Prepare(wrapped, s.Cache);
            Check("wrapped zip: named after its folder", b.Name == "My Mod");
            ModZip.Commit(b, s.Mods);
            Check("wrapped zip: top folder removed", File.Exists(Path.Combine(s.Mods, "My Mod", "textures", "a.png")));

            var top = MakeZip(s, "only-files.zip", z => Entry(z, "files/x", "x"));
            var c = ModZip.Prepare(top, s.Cache);
            Check("a zip of just files/: not treated as wrapped", c.Name == "only-files");
            ModZip.Discard(c);
            Check("discard leaves mods alone", !Directory.Exists(Path.Combine(s.Mods, "only-files")));

            // A patch mod from a zip goes on to build.
            var patch = MakeZip(s, "crate.zip", z =>
            {
                Entry(z, PatchRecipe.FileName, s.Crate("crate"));
                Entry(z, "assets/m.modl", Fixtures.Model("crate"));
                Entry(z, "assets/t.rwtx", Fixtures.Texture("crate"));
            });
            ModZip.Commit(ModZip.Prepare(patch, s.Cache), s.Mods);
            Check("a patch mod from a zip builds", s.Ensure().Mods["crate"].State == PatchState.Ready && File.Exists(s.OutputH002));

            // Replacing.
            var v2 = MakeZip(s, "flat-mod.zip", z => Entry(z, "files/new.txt", "2"));
            var d = ModZip.Prepare(v2, s.Cache);
            Check("replace: seen as existing", ModZip.Exists(d, s.Mods));
            ModZip.Commit(d, s.Mods);
            Check("replace: new in, old gone", File.Exists(Path.Combine(s.Mods, "flat-mod", "files", "new.txt")) && !File.Exists(Path.Combine(s.Mods, "flat-mod", "readme.txt")));

            // A staging folder left by a killed install goes on the next one, but not one holding a mod being replaced.
            var leftover = Path.Combine(s.Cache, "mod-install", "dead", "x");
            var keeping = Path.Combine(s.Cache, "mod-install", "kept", "old");
            Directory.CreateDirectory(leftover);
            Directory.CreateDirectory(keeping);
            ModZip.Discard(ModZip.Prepare(flat, s.Cache));
            Check("killed install: cleaned up", !Directory.Exists(leftover));
            Check("killed install: a replaced mod is kept", Directory.Exists(keeping));
        }
    }

    static string Recipe(string file) => $$"""
        { "format": 1, "levels": { "h002": { "base_sha256": "{{new string('A', 64)}}", "add": [ { "type": "MODL", "name": "m", "file": "{{file}}" } ] } } }
        """;

    // Rewrites the uncompressed size in a zip's central directory to 1, keeping the real data behind it.
    static void Lie(string path)
    {
        var d = File.ReadAllBytes(path);
        for (int i = 0; i + 46 < d.Length; i++)
        {
            if (d[i] != 'P' || d[i + 1] != 'K' || d[i + 2] != 1 || d[i + 3] != 2) continue;
            BinaryPrimitives.WriteUInt32LittleEndian(d.AsSpan(i + 24), 1);
        }
        File.WriteAllBytes(path, d);
    }
}

static class DiscModes
{
    public static int RoundTrip(string disc)
    {
        int same = 0, different = 0, modelsFine = 0, modelsRefused = 0, otherVersion = 0;
        var flags = new Dictionary<(string, uint), int>();
        foreach (var f in Levels(disc))
        {
            var bytes = File.ReadAllBytes(f);
            var hip = new HipFile(bytes);
            if (hip.Build().AsSpan().SequenceEqual(bytes)) same++;
            else { different++; Console.WriteLine("DIFFERENT " + f); }
            foreach (var a in hip.Assets)
            {
                if (a.TypeName is "MODL" or "RWTX" or "SIMP")
                {
                    flags.TryGetValue((a.TypeName, a.Flags), out var n);
                    flags[(a.TypeName, a.Flags)] = n + 1;
                }
                if (a.TypeName != "MODL") continue;
                // The check on a mod's model has to pass what the game itself ships, apart from the disc's few models of another RenderWare version: format 1 only takes 3.1.
                try { RwCheck.Model(a.Data.ToArray(), a.Name); modelsFine++; }
                catch (PatchException e) when (a.Data.Length >= 12 && BinaryPrimitives.ReadUInt32LittleEndian(a.Data.Span[8..]) != 0x310)
                {
                    otherVersion++;
                    Console.WriteLine($"Another version, {BinaryPrimitives.ReadUInt32LittleEndian(a.Data.Span[8..]):X}: {Path.GetFileName(f)} {a.Id:X8} {e.Message}");
                }
                catch (PatchException e) { modelsRefused++; Console.WriteLine($"REFUSED {Path.GetFileName(f)} {a.Id:X8}: {e.Message}"); }
            }
        }
        Console.WriteLine($"{same} levels rebuilt identically, {different} different");
        Console.WriteLine($"{modelsFine} disc models pass the model check, {otherVersion} of another RenderWare version, {modelsRefused} refused");
        foreach (var f in flags.OrderBy(f => f.Key)) Console.WriteLine($"{f.Key.Item1} flags {f.Key.Item2:X}: {f.Value}");
        return different == 0 && modelsRefused == 0 ? 0 : 1;
    }

    // Generated assets into every level that has the layers, for reference.py to repeat with hipedit.py.
    public static int Export(string disc, string work)
    {
        Directory.CreateDirectory(work);
        var model = Fixtures.Model("reference", 500);
        var texture = Fixtures.Texture("reference.RW3", 64, 64);
        var simple = new byte[Fixtures.SimpleSize];
        for (int i = 0; i < simple.Length; i++) simple[i] = (byte)i;
        File.WriteAllBytes(Path.Combine(work, "a.modl"), model);
        File.WriteAllBytes(Path.Combine(work, "a.rwtx"), texture);
        File.WriteAllBytes(Path.Combine(work, "a.simp"), simple);
        var lines = new List<string>();
        foreach (var f in Levels(disc))
        {
            var hip = new HipFile(File.ReadAllBytes(f));
            if (!hip.HasLayer(0) || !hip.HasLayer(1) || !hip.HasLayer(3)) continue;
            hip.Add(HipFile.LayerTextures, "RWTX", HipFile.HashName("reference.RW3"), texture, "reference.RW3", 6, 0x40);
            hip.Add(HipFile.LayerModels, "MODL", HipFile.HashName("reference"), model, "reference", 5, 0x10);
            hip.Add(HipFile.LayerDefault, "SIMP", HipFile.HashName("REFERENCE OBJ"), simple, "REFERENCE OBJ", 2, 0x20);
            lines.Add(Path.GetFileName(f) + " " + Fixtures.Sha(hip.Build()));
        }
        File.WriteAllLines(Path.Combine(work, "csharp.txt"), lines);
        Console.WriteLine($"{lines.Count} levels written to {Path.Combine(work, "csharp.txt")}");
        return 0;
    }

    public static int Recipe(string[] args)
    {
        string Arg(string name) => args[Array.IndexOf(args, name) + 1];
        var level = args.Contains("--level") ? Arg("--level") : "h002";
        var root = Path.Combine(Path.GetTempPath(), "hip-patch-tests", Guid.NewGuid().ToString("N"));
        var mods = Path.Combine(root, "mods");
        var modDir = Path.Combine(mods, Path.GetFileName(Arg("--recipe").TrimEnd('\\', '/')));
        try
        {
            CopyTree(Arg("--recipe"), modDir);
            var r = PatchBuilder.Ensure(mods, Arg("--disc"), Path.Combine(root, "Cache"));
            foreach (var m in r.Mods) Console.WriteLine($"{m.Key}: {m.Value.State} {m.Value.Text}");
            if (r.Error != null) Console.WriteLine("error: " + r.Error);
            var built = Directory.EnumerateFiles(Path.Combine(mods, PatchBuilder.OutputFolder), level + ".HIP", new EnumerationOptions { RecurseSubdirectories = true, MatchCasing = MatchCasing.CaseInsensitive }).Single();
            var sha = Fixtures.Sha(File.ReadAllBytes(built));
            Console.WriteLine($"{new FileInfo(built).Length} bytes, {sha}");
            bool ok = sha.Equals(Arg("--expect"), StringComparison.OrdinalIgnoreCase);
            Console.WriteLine(ok ? "SAME as expected" : "DIFFERENT from " + Arg("--expect"));
            return ok ? 0 : 1;
        }
        finally { Directory.Delete(root, true); }
    }

    static void CopyTree(string from, string to)
    {
        Directory.CreateDirectory(to);
        foreach (var f in Directory.GetFiles(from)) File.Copy(f, Path.Combine(to, Path.GetFileName(f)));
        foreach (var d in Directory.GetDirectories(from)) CopyTree(d, Path.Combine(to, Path.GetFileName(d)));
    }

    static IEnumerable<string> Levels(string disc) =>
        Directory.EnumerateFiles(Path.Combine(disc, "files"), "*.hip", new EnumerationOptions { RecurseSubdirectories = true, MatchCasing = MatchCasing.CaseInsensitive }).OrderBy(f => f, StringComparer.OrdinalIgnoreCase);
}
