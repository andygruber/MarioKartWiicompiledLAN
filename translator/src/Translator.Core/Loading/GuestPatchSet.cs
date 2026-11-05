using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace Translator.Core.Loading;

/// <summary>One verified 32-bit word replacement in the loaded program image.</summary>
public readonly record struct GuestPatchWord(uint Address, uint Expected, uint Replacement, int Line);

/// <summary>
/// A named list of static word patches applied to the loaded DOL/REL image before anything is
/// translated or emitted. Translated code cannot be rewritten at run time, so code mods that poke
/// instructions (BrainSlug/Gecko style) are expressed as a patch set instead and get translated as
/// if the game had shipped with them.
///
/// Text format, one patch per line, '#' starts a comment:
/// <code>
///   set lan                               # required: the patch set name
///   800CCC74 906D96A8 -> 60000000         # expected original word -> replacement word
///   805D2114 4BF4759D -> bl 819F0000      # replacement as a relative branch (b / bl)
/// </code>
/// Every expected word is verified against the image, so a patch set never silently applies to a
/// different game revision. Addresses outside the DOL/REL (for example native trampoline stubs in
/// otherwise unused RAM) are allowed and expect <c>00000000</c>.
/// </summary>
public sealed class GuestPatchSet
{
    private GuestPatchSet(string name, string sourcePath, IReadOnlyList<GuestPatchWord> words, string sha256)
    {
        Name = name;
        SourcePath = sourcePath;
        Words = words;
        Sha256 = sha256;
    }

    public string Name { get; }
    public string SourcePath { get; }
    public IReadOnlyList<GuestPatchWord> Words { get; }
    public string Sha256 { get; }

    public static GuestPatchSet Load(string path)
    {
        var fullPath = Path.GetFullPath(path);
        var bytes = File.ReadAllBytes(fullPath);
        var text = Encoding.UTF8.GetString(bytes);
        string? name = null;
        var words = new List<GuestPatchWord>();
        var seen = new HashSet<uint>();
        var lineNumber = 0;
        foreach (var rawLine in text.Split('\n'))
        {
            lineNumber++;
            var line = rawLine;
            var comment = line.IndexOf('#');
            if (comment >= 0)
                line = line[..comment];
            line = line.Trim();
            if (line.Length == 0)
                continue;

            var tokens = line.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            if (tokens[0] == "set")
            {
                if (tokens.Length != 2 || name is not null)
                    throw Error(fullPath, lineNumber, "expected exactly one 'set <name>' line");
                name = tokens[1];
                continue;
            }

            // <address> <expected> -> <word> | <address> <expected> -> b|bl <target>
            if (tokens.Length < 4 || tokens[2] != "->")
                throw Error(fullPath, lineNumber, "expected '<address> <expected> -> <replacement>'");
            var address = ParseHex(tokens[0], fullPath, lineNumber);
            var expected = ParseHex(tokens[1], fullPath, lineNumber);
            if ((address & 3) != 0)
                throw Error(fullPath, lineNumber, $"address 0x{address:X8} is not word aligned");
            uint replacement;
            if (tokens.Length == 4)
            {
                replacement = ParseHex(tokens[3], fullPath, lineNumber);
            }
            else if (tokens.Length == 5 && (tokens[3] == "b" || tokens[3] == "bl"))
            {
                var target = ParseHex(tokens[4], fullPath, lineNumber);
                var displacement = unchecked((int)(target - address));
                if ((target & 3) != 0 || displacement < -0x02000000 || displacement >= 0x02000000)
                    throw Error(fullPath, lineNumber, $"branch target 0x{target:X8} is out of range");
                replacement = 0x48000000u | ((uint)displacement & 0x03FFFFFCu) | (tokens[3] == "bl" ? 1u : 0u);
            }
            else
            {
                throw Error(fullPath, lineNumber, "unrecognised replacement");
            }
            if (!seen.Add(address))
                throw Error(fullPath, lineNumber, $"address 0x{address:X8} is patched twice");
            words.Add(new GuestPatchWord(address, expected, replacement, lineNumber));
        }

        if (name is null)
            throw new InvalidDataException($"{fullPath}: missing 'set <name>' line");
        return new GuestPatchSet(name, fullPath, words, Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant());
    }

    /// <summary>Verifies and applies every word to <paramref name="memory"/> (big-endian guest RAM).</summary>
    public void ApplyTo(byte[] memory, uint memoryBase)
    {
        foreach (var word in Words)
        {
            var offset = (long)word.Address - memoryBase;
            if (offset < 0 || offset + 4 > memory.Length)
                throw Error(SourcePath, word.Line, $"address 0x{word.Address:X8} is outside guest RAM");
            var span = memory.AsSpan((int)offset, 4);
            var actual = BinaryPrimitives.ReadUInt32BigEndian(span);
            if (actual != word.Expected)
                throw Error(SourcePath, word.Line,
                    $"0x{word.Address:X8} holds 0x{actual:X8}, expected 0x{word.Expected:X8} " +
                    "(the patch set does not match this game revision)");
            BinaryPrimitives.WriteUInt32BigEndian(span, word.Replacement);
        }
    }

    /// <summary>A new image with this set applied; the ranges are unchanged and the hash is recomputed.</summary>
    public ProgramImage ApplyTo(ProgramImage image)
    {
        var memory = (byte[])image.Memory.Clone();
        ApplyTo(memory, image.MemoryBase);
        return new ProgramImage(memory, image.UsedRange, image.DolRange, image.RelRange,
            ChecksumUtilities.Sha256Hex(memory), image.MemoryBase);
    }

    private static uint ParseHex(string token, string path, int line)
    {
        var text = token.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? token[2..] : token;
        if (!uint.TryParse(text, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var value))
            throw Error(path, line, $"'{token}' is not a hexadecimal word");
        return value;
    }

    private static InvalidDataException Error(string path, int line, string message) =>
        new($"{path}:{line}: {message}");
}
