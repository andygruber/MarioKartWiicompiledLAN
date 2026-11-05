using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using Translator.Core.Loading;

namespace Translator.Core.IO;

internal sealed record AssemblyBlob(string FileName, string Symbol, ReadOnlyMemory<byte> Data, string Comment);

internal static class AssemblyBlobWriter
{
    // Object format the generated assembly is for: "windows", "macos" or "linux", or the host's.
    private static readonly string? TargetOverride =
        Environment.GetEnvironmentVariable("MKW_ASM_TARGET")?.Trim().ToLowerInvariant() is { Length: > 0 } target
            ? target is "windows" or "macos" or "linux"
                ? target
                : throw new InvalidOperationException($"MKW_ASM_TARGET must be windows, macos or linux (got '{target}').")
            : null;
    private static bool TargetIsWindows => TargetOverride is null ? OperatingSystem.IsWindows() : TargetOverride == "windows";
    private static bool TargetIsMacOS => TargetOverride is null ? OperatingSystem.IsMacOS() : TargetOverride == "macos";

    public static void Write(
        string assemblyPath,
        string blobDirectory,
        string blobReferenceDirectory,
        IReadOnlyList<AssemblyBlob> blobs,
        params string[] headerLines)
    {
        Directory.CreateDirectory(blobDirectory);
        var expectedFiles = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var assembly = new StringBuilder();
        foreach (var header in headerLines) assembly.AppendLine(header);
        // PE/COFF (Windows), Mach-O (macOS), and ELF (Linux) spell a read-only data section
        // differently in GNU-as syntax. The build normally targets whichever platform the
        // translator itself runs on; a cross build (Launcher/release/build-windows.sh) names its
        // target with MKW_ASM_TARGET instead.
        assembly.AppendLine(TargetIsWindows
            ? ".section .rdata,\"dr\""
            : TargetIsMacOS
                ? ".section __TEXT,__const"
                : ".section .rodata,\"a\",@progbits");
        assembly.AppendLine();

        foreach (var blob in blobs)
        {
            var blobPath = Path.Combine(blobDirectory, blob.FileName);
            expectedFiles.Add(Path.GetFullPath(blobPath));
            FileOutput.WriteBytesIfChanged(blobPath, blob.Data.Span);
            var hash = ChecksumUtilities.Sha256Hex(blob.Data.Span);
            assembly.AppendLine($"// {blob.Comment}; sha256={hash}");
            assembly.AppendLine(".p2align 4");
            // C/C++ external symbols carry a leading underscore in Mach-O,
            // unlike ELF and COFF.  The generated C++ still names the symbol
            // without that ABI decoration, so emit the platform spelling here.
            var assemblySymbol = TargetIsMacOS ? $"_{blob.Symbol}" : blob.Symbol;
            assembly.AppendLine($".globl {assemblySymbol}");
            assembly.AppendLine($"{assemblySymbol}:");
            var referencePath = Path.Combine(blobReferenceDirectory, blob.FileName);
            assembly.AppendLine($".incbin \"{SanitizeAssemblyPath(referencePath)}\"");
            assembly.AppendLine();
        }

        foreach (var stalePath in Directory.EnumerateFiles(blobDirectory, "*.bin"))
        {
            if (!expectedFiles.Contains(Path.GetFullPath(stalePath))) File.Delete(stalePath);
        }
        if (!TargetIsWindows && !TargetIsMacOS)
        {
            // Absence of a .note.GNU-stack section makes the linker assume the oldest, most
            // conservative default for this object (an executable stack) and warn about it; this
            // file has no code needing one, so mark it explicitly like every other GNU-as ELF
            // object linked into the binary already does (the norm on modern toolchains, just not
            // producible without an explicit section since this file is hand-assembled, not
            // compiler-emitted).
            assembly.AppendLine(".section .note.GNU-stack,\"\",@progbits");
        }
        FileOutput.WriteTextIfChanged(assemblyPath, assembly.ToString());
    }

    private static string SanitizeAssemblyPath(string path) => Path.GetFullPath(path).Replace('\\', '/');
}
