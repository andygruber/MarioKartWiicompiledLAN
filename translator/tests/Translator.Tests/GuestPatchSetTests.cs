using System.Buffers.Binary;
using Translator.Core.Loading;
using Xunit;

namespace Translator.Tests;

public sealed class GuestPatchSetTests
{
    private static string WriteSet(string text)
    {
        var path = Path.Combine(Path.GetTempPath(), $"mkw-guest-patches-{Guid.NewGuid():N}.txt");
        File.WriteAllText(path, text);
        return path;
    }

    private static byte[] Ram(params (uint Address, uint Word)[] words)
    {
        var ram = new byte[0x1000];
        foreach (var (address, word) in words)
            BinaryPrimitives.WriteUInt32BigEndian(ram.AsSpan((int)(address - 0x80000000u), 4), word);
        return ram;
    }

    private static uint Word(byte[] ram, uint address) =>
        BinaryPrimitives.ReadUInt32BigEndian(ram.AsSpan((int)(address - 0x80000000u), 4));

    [Fact]
    public void ParsesWordsAndEncodesRelativeBranches()
    {
        var path = WriteSet("""
            # comment
            set lan
            80000100 906D96A8 -> 60000000   # trailing comment
            80000104 4BF4759D -> bl 80000004
            80000108 00000000 -> b 80000208
            """);
        try
        {
            var set = GuestPatchSet.Load(path);
            Assert.Equal("lan", set.Name);
            Assert.Equal(3, set.Words.Count);
            Assert.Equal(0x60000000u, set.Words[0].Replacement);
            Assert.Equal(0x4BFFFF01u, set.Words[1].Replacement); // bl -0x100
            Assert.Equal(0x48000100u, set.Words[2].Replacement); // b +0x100

            var ram = Ram((0x80000100u, 0x906D96A8u), (0x80000104u, 0x4BF4759Du));
            set.ApplyTo(ram, 0x80000000u);
            Assert.Equal(0x60000000u, Word(ram, 0x80000100u));
            Assert.Equal(0x4BFFFF01u, Word(ram, 0x80000104u));
            Assert.Equal(0x48000100u, Word(ram, 0x80000108u));
        }
        finally
        {
            File.Delete(path);
        }
    }

    [Fact]
    public void RejectsAnImageWhoseOriginalWordDiffers()
    {
        var path = WriteSet("set lan\n80000100 906D96A8 -> 60000000\n");
        try
        {
            var set = GuestPatchSet.Load(path);
            var ram = Ram((0x80000100u, 0x12345678u));
            var error = Assert.Throws<InvalidDataException>(() => set.ApplyTo(ram, 0x80000000u));
            Assert.Contains("expected 0x906D96A8", error.Message);
            Assert.Equal(0x12345678u, Word(ram, 0x80000100u));
        }
        finally
        {
            File.Delete(path);
        }
    }

    [Theory]
    [InlineData("80000100 00000000 -> 60000000\n")]                       // no set line
    [InlineData("set a\nset b\n")]                                         // two set lines
    [InlineData("set lan\n80000102 00000000 -> 60000000\n")]               // unaligned
    [InlineData("set lan\n80000100 00000000 60000000\n")]                  // missing arrow
    [InlineData("set lan\n80000100 0 -> 1\n80000100 0 -> 2\n")]            // patched twice
    [InlineData("set lan\n80000100 00000000 -> bl 84000100\n")]            // out of branch range
    public void RejectsMalformedSets(string text)
    {
        var path = WriteSet(text);
        try
        {
            Assert.Throws<InvalidDataException>(() => GuestPatchSet.Load(path));
        }
        finally
        {
            File.Delete(path);
        }
    }

    [Fact]
    public void RepositoryLanPatchSetIsWellFormed()
    {
        var path = Path.Combine(ProjectPathsForTests.FindRepositoryRoot(), "projects", "mkwii", "lan_patches.txt");
        var set = GuestPatchSet.Load(path);
        Assert.Equal("lan", set.Name);
        Assert.Equal(42, set.Words.Count);
        // The runtime's LAN natives (runtime/src/hle/net/lan_mode.cpp) own these trampolines.
        foreach (var trampoline in new[] { 0x819F0000u, 0x819F0010u, 0x819F0020u, 0x819F0030u })
            Assert.Contains(set.Words, word => word.Address == trampoline && word.Replacement == 0x4E800020u);
    }
}
