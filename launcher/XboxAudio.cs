using System.Buffers.Binary;
using FrightWorks.ModPatches;

namespace FrightWorks;

// The Xbox version's sound, from the player's own Xbox disc image. The music, voices and effects are the GameCube's
// (same assets, same sample rates and lengths), but stored as plain 16-bit PCM where the GameCube has DSP-ADPCM, whose
// 4-bit samples add a flat hiss: 51-62 dB under the music and only 26-37 dB under the 22 kHz voice clips. This pairs
// each level's SND (resident sounds) and SNDS (streamed music and speech) assets by asset ID and writes, into
// <UserData>/xbox/audio:
//   audio.pcm  the Xbox samples, one asset after another (int16 little-endian, each start 4 KB aligned);
//   audio.idx  for each asset its GameCube ID, size and sample count and where its samples are; and for each 12000-byte
//              chunk of the GameCube asset (the size the game streams in) the hash of its first 64 bytes and of the
//              whole chunk. The game recognises the GameCube bytes it copies into ARAM by these hashes and plays the
//              Xbox samples for them (wiicompiled/runtime/src/hle/audio/xbox_audio.cpp).
// The copies are the player's disc data, like the extracted disc: never commit, share or ship them.
// No window code here, so tools/hip-patch-tests can run it.
static class XboxAudio
{
    public const int Chunk = 0x2EE0;  // the game's stream chunk (0.4375 s of 48 kHz ADPCM)
    public const int Head = 64;
    const int Version = 1;

    public static string Dir(string userData) => Path.Combine(userData, "xbox", "audio");

    // How many sounds are set up (0 when none or unreadable).
    public static int Count(string userData)
    {
        try
        {
            var idx = Path.Combine(Dir(userData), "audio.idx");
            if (!File.Exists(idx)) return 0;
            using var f = File.OpenRead(idx);
            var head = new byte[12];
            return f.Read(head) == 12 && head.AsSpan(0, 4).SequenceEqual("FWXA"u8) ? (int)BinaryPrimitives.ReadUInt32LittleEndian(head.AsSpan(8)) : 0;
        }
        catch (Exception) { return 0; }
    }

    public static void Remove(string userData) => TryDelete(Dir(userData));

    // Writes the sound; on any problem the folder is left as it was. progress: (levels done, levels to do, name).
    // Returns how many sounds were written.
    public static int Copy(Ps2Cutscenes.IDiscImage xbox, string gcDisc, string userData, Action<int, int, string>? progress = null,
                           CancellationToken cancel = default)
    {
        var xboxLevels = new Dictionary<string, Ps2Cutscenes.DiscEntry>(StringComparer.OrdinalIgnoreCase);
        foreach (var e in xbox.Files())
            if (e.Name.EndsWith(".HIP", StringComparison.OrdinalIgnoreCase)) xboxLevels[e.Name] = e;
        var gcFiles = Path.Combine(gcDisc, "files");
        var gcLevels = Directory.EnumerateFiles(gcFiles, "*.hip", new EnumerationOptions { RecurseSubdirectories = true, MatchCasing = MatchCasing.CaseInsensitive })
                                .Where(p => xboxLevels.ContainsKey(Path.GetFileName(p))).OrderBy(p => p, StringComparer.OrdinalIgnoreCase).ToList();
        var dir = Dir(userData);
        var staging = dir + ".new";
        TryDelete(staging);
        Directory.CreateDirectory(staging);
        var seen = new HashSet<uint>();
        var assets = new List<(uint Id, uint GcBytes, uint Samples, uint FirstChunk, ulong PcmOffset)>();
        var chunks = new List<(ulong Head, ulong Hash, uint Asset, uint Offset, uint Length)>();
        int done = 0;
        try
        {
            using (var pcm = new FileStream(Path.Combine(staging, "audio.pcm"), FileMode.CreateNew, FileAccess.Write))
            {
                foreach (var gcPath in gcLevels)
                {
                    cancel.ThrowIfCancellationRequested();
                    var name = Path.GetFileName(gcPath);
                    progress?.Invoke(done++, gcLevels.Count, name);
                    HipFile gc, xb;
                    try { gc = new HipFile(File.ReadAllBytes(gcPath)); xb = new HipFile(xbox.Read(xboxLevels[name])); }
                    catch (InvalidDataException) { continue; }
                    var xboxSounds = new Dictionary<uint, ReadOnlyMemory<byte>>();
                    foreach (var a in xb.Assets) if (a.TypeName is "SND " or "SNDS") xboxSounds[a.Id] = a.Data;
                    foreach (var a in gc.Assets)
                    {
                        if (a.TypeName is not ("SND " or "SNDS") || !xboxSounds.TryGetValue(a.Id, out var xd) || seen.Contains(a.Id)) continue;
                        var g = a.Data.Span;
                        // Same sound: the Xbox has as many samples as the ADPCM frames hold (14 a frame), less the
                        // GameCube's padding to 32 bytes (up to 3 frames and 13 samples). 32 sounds on the USA disc are a
                        // quarter to half as long on the Xbox (another recording or rate): those stay the GameCube's.
                        long frames = g.Length / 8, samples = xd.Length / 2;
                        if (g.Length < 8 || samples < (frames - 4) * 14 || samples > frames * 14) continue;
                        seen.Add(a.Id);
                        uint index = (uint)assets.Count;
                        long at = (pcm.Position + 4095) / 4096 * 4096;
                        pcm.SetLength(at);
                        pcm.Position = at;
                        pcm.Write(xd.Span[..(int)(samples * 2)]);
                        assets.Add((a.Id, (uint)g.Length, (uint)samples, (uint)chunks.Count, (ulong)at));
                        for (int off = 0; off < g.Length; off += Chunk)
                        {
                            int len = Math.Min(Chunk, g.Length - off);
                            var c = g.Slice(off, len);
                            chunks.Add((XboxTextures.XxHash64(c[..Math.Min(Head, len)]), XboxTextures.XxHash64(c), index, (uint)off, (uint)len));
                        }
                    }
                }
            }
            progress?.Invoke(done, gcLevels.Count, "");
            if (assets.Count == 0) { TryDelete(staging); return 0; }
            using (var idx = new BinaryWriter(File.Create(Path.Combine(staging, "audio.idx"))))
            {
                idx.Write("FWXA"u8);
                idx.Write((uint)Version);
                idx.Write((uint)assets.Count);
                idx.Write((uint)chunks.Count);
                foreach (var a in assets) { idx.Write(a.Id); idx.Write(a.GcBytes); idx.Write(a.Samples); idx.Write(a.FirstChunk); idx.Write(a.PcmOffset); }
                foreach (var c in chunks) { idx.Write(c.Head); idx.Write(c.Hash); idx.Write(c.Asset); idx.Write(c.Offset); idx.Write(c.Length); idx.Write(0u); }
            }
            TryDelete(dir);
            Directory.CreateDirectory(Path.GetDirectoryName(dir)!);
            Directory.Move(staging, dir);
            return assets.Count;
        }
        catch (Exception) { TryDelete(staging); throw; }
    }

    static void TryDelete(string dir)
    {
        try { if (Directory.Exists(dir)) Directory.Delete(dir, true); }
        catch (Exception) { }
    }
}
