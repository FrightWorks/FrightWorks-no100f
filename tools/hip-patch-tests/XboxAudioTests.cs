using System.Buffers.Binary;
using FrightWorks;
using HipPatchTests;

// The launcher's Xbox sound copy (launcher/XboxAudio.cs) on an Xbox disc image and a GameCube disc made in code.
static partial class Tests
{
    static void XboxAudioCopy()
    {
        var root = Path.Combine(Path.GetTempPath(), "hip-patch-tests", Guid.NewGuid().ToString("N"));
        try
        {
            var gcDisc = Path.Combine(root, "Disc");
            var userData = Path.Combine(root, "UserData");
            Directory.CreateDirectory(Path.Combine(gcDisc, "files", "hb"));
            Directory.CreateDirectory(userData);
            // GC: "music" (3 chunks and a bit of ADPCM, 0x2EE0 each) and "bark" (one short frame run); "odd" has an Xbox
            // copy of the wrong length (left out).
            var music = Enumerable.Range(0, 3 * XboxAudio.Chunk + 800).Select(i => (byte)(i * 31 + 7)).ToArray();
            var bark = Enumerable.Range(0, 160).Select(i => (byte)(i * 13 + 1)).ToArray();
            var odd = new byte[80];
            File.WriteAllBytes(Path.Combine(gcDisc, "files", "hb", "h001.HIP"), Fixtures.Level(1, new[]
            {
                new Fixtures.A(0, "SNDS", "music-L", music, 0), new Fixtures.A(0, "SND ", "bark", bark, 0), new Fixtures.A(0, "SND ", "odd", odd, 0),
            }));
            byte[] Pcm(int samples, int seed) => Enumerable.Range(0, samples * 2).Select(i => (byte)(i * seed)).ToArray();
            var musicPcm = Pcm(music.Length / 8 * 14, 3);
            var barkPcm = Pcm(bark.Length / 8 * 14 - 5, 5);  // a few samples of padding in the last ADPCM frame
            var xboxLevel = Fixtures.Level(2, new[]
            {
                new Fixtures.A(0, "SNDS", "music-L", musicPcm, 0), new Fixtures.A(0, "SND ", "bark", barkPcm, 0), new Fixtures.A(0, "SND ", "odd", new byte[4], 0),
            });
            var iso = Path.Combine(root, "xbox.iso");
            File.WriteAllBytes(iso, Xiso(0, ("h0", "h001.HIP", xboxLevel)));

            int n;
            using (var disc = Ps2Cutscenes.OpenImage(iso)) n = XboxAudio.Copy(disc, gcDisc, userData);
            Check("Xbox audio: the two matching sounds are written", n == 2 && XboxAudio.Count(userData) == 2, $"{n}");
            var idx = File.ReadAllBytes(Path.Combine(XboxAudio.Dir(userData), "audio.idx"));
            var pcm = File.ReadAllBytes(Path.Combine(XboxAudio.Dir(userData), "audio.pcm"));
            uint U32(int at) => BinaryPrimitives.ReadUInt32LittleEndian(idx.AsSpan(at));
            ulong U64(int at) => BinaryPrimitives.ReadUInt64LittleEndian(idx.AsSpan(at));
            Check("Xbox audio: index header", idx.AsSpan(0, 4).SequenceEqual("FWXA"u8) && U32(4) == 1 && U32(8) == 2 && U32(12) == 5, $"{U32(8)} {U32(12)}");
            // Assets in level order (the fixture sorts by name hash); find each by its GameCube ID.
            int AssetAt(string name) => Enumerable.Range(0, 2).Select(i => 16 + 24 * i).First(o => U32(o) == FrightWorks.ModPatches.HipFile.HashName(name));
            int m = AssetAt("music-L"), b = AssetAt("bark");
            Check("Xbox audio: sizes and sample counts", U32(m + 4) == music.Length && U32(m + 8) == musicPcm.Length / 2 && U32(b + 4) == bark.Length && U32(b + 8) == barkPcm.Length / 2);
            Check("Xbox audio: samples 4 KB aligned and byte for byte",
                  U64(m + 16) % 4096 == 0 && U64(b + 16) % 4096 == 0 &&
                  pcm.AsSpan((int)U64(m + 16), musicPcm.Length).SequenceEqual(musicPcm) && pcm.AsSpan((int)U64(b + 16), barkPcm.Length).SequenceEqual(barkPcm));
            // Chunks: the music's four (three whole, then 800 bytes) with their hashes, in order after its first chunk.
            int chunks = 16 + 24 * 2, first = (int)U32(m + 12);
            bool ok = true;
            for (int i = 0; i < 4; i++)
            {
                int at = chunks + 32 * (first + i), off = i * XboxAudio.Chunk, len = Math.Min(XboxAudio.Chunk, music.Length - off);
                ok &= U64(at) == XboxTextures.XxHash64(music.AsSpan(off, XboxAudio.Head)) && U64(at + 8) == XboxTextures.XxHash64(music.AsSpan(off, len))
                      && U32(at + 20) == off && U32(at + 24) == len;
            }
            Check("Xbox audio: chunk hashes, offsets and lengths", ok);
            XboxAudio.Remove(userData);
            Check("Xbox audio: remove", XboxAudio.Count(userData) == 0 && !Directory.Exists(XboxAudio.Dir(userData)));
        }
        finally
        {
            try { Directory.Delete(root, true); } catch (Exception) { }
        }
    }
}
