# Release copies only. Never pass a build or vcpkg directory to the writer.
Set-StrictMode -Version Latest

if (-not ('QuadDeck.ReleasePrivacy' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace QuadDeck {
public static class ReleasePrivacy {
    private sealed class Section {
        public int Start, Size;
        public uint Rva, VirtualSize, Flags;
    }
    private sealed class Span {
        public int Start, Size;
    }
    private sealed class Marker {
        public int Offset, Width;
    }
    private static readonly Regex Home = new Regex(
        @"[a-z]:[\\/](?:Users|Documents and Settings)[\\/]|/(?:home|Users)/[a-z0-9._-]+/",
        RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
    private static readonly Regex Prefix = new Regex(
        @"^[a-z]:[\\/]Users[\\/][^\\/\x00]+(?=[\\/])",
        RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
    private static readonly Regex Source = new Regex(
        @"^[a-z]:/Users/[^/]+/vcpkg/buildtrees/ffmpeg/src/[^/]+/(?:libavcodec|libavformat|libavutil|libavfilter|libswresample|libswscale)/[a-z0-9_./-]+\.[ch]$",
        RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
    private static readonly Regex FfmpegDll = new Regex(
        @"^(?:avcodec|avformat|avutil|swresample|swscale)-[0-9]+\.dll$",
        RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);

    private static void Require(bool condition, string message) {
        if (!condition) throw new InvalidDataException(message);
    }
    private static void Bounds(byte[] data, long offset, long size) {
        Require(offset >= 0 && size >= 0 && offset <= data.Length && size <= data.Length - offset,
                "Invalid or truncated PE range.");
    }
    private static ushort U16(byte[] data, int offset) {
        Bounds(data, offset, 2); return BitConverter.ToUInt16(data, offset);
    }
    private static uint U32(byte[] data, int offset) {
        Bounds(data, offset, 4); return BitConverter.ToUInt32(data, offset);
    }
    private static int Offset(uint value) {
        Require(value <= int.MaxValue, "PE offset exceeds the supported range.");
        return (int)value;
    }
    private static List<Marker> Markers(byte[] data) {
        var result = new List<Marker>();
        // Latin-1 preserves byte offsets; scan both UTF-16 alignments too.
        string ascii = Encoding.GetEncoding(28591).GetString(data);
        foreach (Match match in Home.Matches(ascii))
            result.Add(new Marker { Offset = match.Index, Width = 1 });
        for (int parity = 0; parity < 2; ++parity) {
            int count = (data.Length - parity) & ~1;
            if (count <= 0) continue;
            string wide = Encoding.Unicode.GetString(data, parity, count);
            foreach (Match match in Home.Matches(wide))
                result.Add(new Marker { Offset = parity + match.Index * 2, Width = 2 });
        }
        return result;
    }
    public static void Verify(byte[] data) {
        Require(Markers(data).Count == 0, "Private home-directory bytes remain in the release file.");
    }
    private static Section Containing(List<Section> sections, int start, int size) {
        foreach (Section section in sections)
            if (start >= section.Start && (long)start + size <= (long)section.Start + section.Size)
                return section;
        throw new InvalidDataException("Private string or debug record lies outside a PE section.");
    }
    private static int FromRva(List<Section> sections, uint rva, int size) {
        foreach (Section section in sections) {
            long delta = (long)rva - section.Rva;
            if (delta >= 0 && delta + size <= section.Size)
                return section.Start + (int)delta;
        }
        throw new InvalidDataException("Invalid PE debug-directory RVA.");
    }
    private static int Character(byte[] data, int offset, int width) {
        return width == 1 ? data[offset] : U16(data, offset);
    }
    private static string Literal(byte[] data, int start, int width, Section section, out int end) {
        end = start;
        int limit = section.Start + section.Size;
        while (end <= limit - width) {
            int c = Character(data, end, width);
            if (c == 0)
                return width == 1 ? Encoding.ASCII.GetString(data, start, end - start)
                                  : Encoding.Unicode.GetString(data, start, end - start);
            Require(c >= 32 && c <= 126, "Private path is not a recognized printable diagnostic literal.");
            end += width;
        }
        throw new InvalidDataException("Unterminated private diagnostic literal.");
    }
    private static uint Checksum(byte[] data, int checksumOffset) {
        ulong sum = 0;
        for (int i = 0; i < data.Length; i += 2) {
            if (i == checksumOffset || i == checksumOffset + 2) continue;
            uint word = data[i];
            if (i + 1 < data.Length) word |= (uint)data[i + 1] << 8;
            sum += word;
            sum = (sum & 0xffff) + (sum >> 16);
        }
        sum = (sum & 0xffff) + (sum >> 16);
        return (uint)(sum + (ulong)data.Length);
    }
    public static byte[] Sanitize(byte[] input, string fileName) {
        var data = (byte[])input.Clone();
        var markers = Markers(data);
        if (markers.Count == 0) return data;
        Require(U16(data, 0) == 0x5a4d, "Private bytes in a non-PE file cannot be rewritten.");
        int pe = Offset(U32(data, 60));
        Bounds(data, pe, 24);
        Require(U32(data, pe) == 0x4550 && U16(data, pe + 4) == 0x8664,
                "Only AMD64 PE images support release path rewriting.");
        int sectionCount = U16(data, pe + 6), optionalSize = U16(data, pe + 20);
        int optional = pe + 24;
        Bounds(data, optional, optionalSize);
        Require(optionalSize >= 168 && U16(data, optional) == 0x20b && U32(data, optional + 108) >= 7,
                "Invalid PE32+ optional header.");
        Require(U32(data, optional + 144) == 0 && U32(data, optional + 148) == 0,
                "Signed images must be rebuilt without private paths; their bytes cannot be rewritten.");
        Require((U16(data, optional + 70) & 0x80) == 0,
                "Force-integrity images cannot be rewritten.");
        int table = optional + optionalSize;
        Bounds(data, table, (long)sectionCount * 40);
        var sections = new List<Section>();
        for (int i = 0; i < sectionCount; ++i) {
            int entry = table + i * 40;
            var section = new Section {
                Start = Offset(U32(data, entry + 20)), Size = Offset(U32(data, entry + 16)),
                Rva = U32(data, entry + 12), VirtualSize = U32(data, entry + 8), Flags = U32(data, entry + 36)
            };
            Bounds(data, section.Start, section.Size);
            if (section.Size != 0) {
                Require(section.Start >= table + sectionCount * 40, "PE section overlaps its headers.");
                foreach (Section other in sections)
                    Require((long)section.Start + section.Size <= other.Start ||
                            (long)other.Start + other.Size <= section.Start, "Overlapping PE sections.");
                sections.Add(section);
            }
        }
        var pdbPaths = new List<Span>();
        uint debugRva = U32(data, optional + 160);
        int debugSize = Offset(U32(data, optional + 164));
        Require((debugRva == 0) == (debugSize == 0) && debugSize % 28 == 0,
                "Invalid PE debug directory.");
        if (debugSize != 0) {
            int debug = FromRva(sections, debugRva, debugSize);
            for (int i = 0; i < debugSize; i += 28) {
                int entry = debug + i;
                if (U32(data, entry + 12) != 2) continue;
                int size = Offset(U32(data, entry + 16)), raw = Offset(U32(data, entry + 24));
                Bounds(data, raw, size);
                Require(size >= 4, "Truncated CodeView record.");
                if (U32(data, raw) != 0x53445352) continue; // Unsupported formats stay untouched.
                Require(size >= 25, "Truncated RSDS record.");
                Section section = Containing(sections, raw, size);
                int end;
                Literal(data, raw + 24, 1, section, out end);
                Require(end < raw + size, "RSDS filename exceeds its record.");
                pdbPaths.Add(new Span { Start = raw + 24, Size = end - raw - 24 });
            }
        }
        bool ffmpeg = FfmpegDll.IsMatch(Path.GetFileName(fileName));
        foreach (Marker marker in markers) {
            Section section = Containing(sections, marker.Offset, marker.Width);
            Require((section.Flags & 0x40000040) == 0x40000040 && (section.Flags & 0xa0000000) == 0,
                    "Private path is not in read-only, non-executable initialized data.");
            int end;
            string suffix = Literal(data, marker.Offset, marker.Width, section, out end);
            Match prefix = Prefix.Match(suffix);
            Require(prefix.Success, "Unrecognized private home-path representation.");
            bool recognized = false;
            foreach (Span pdb in pdbPaths)
                if (marker.Width == 1 && marker.Offset == pdb.Start && end == pdb.Start + pdb.Size)
                    recognized = suffix.EndsWith(".pdb", StringComparison.OrdinalIgnoreCase);
            if (!recognized && ffmpeg) {
                recognized = Source.IsMatch(suffix.Replace('\\', '/'));
                if (!recognized && marker.Width == 1) {
                    int start = marker.Offset;
                    while (start > section.Start && data[start - 1] >= 32 && data[start - 1] <= 126) --start;
                    Require(start > section.Start && data[start - 1] == 0,
                            "FFmpeg configuration has no bounded string start.");
                    string configuration = Encoding.ASCII.GetString(data, start, end - start);
                    recognized = configuration.StartsWith("--prefix=", StringComparison.Ordinal) &&
                                 configuration.Contains("--toolchain=msvc") &&
                                 configuration.Contains("--target-os=win32") &&
                                 configuration.Contains("--enable-shared");
                }
            }
            Require(recognized, "Private path is outside supported CodeView or FFmpeg diagnostic literals.");
            // Preserve every byte offset, terminator, pooled suffix, RSDS GUID and age.
            // 'build' is the same length as 'Users'; the account is neutral padding.
            string replacement = prefix.Value.Substring(0, 3) + "build" + prefix.Value[8] +
                                 new string('_', prefix.Length - 9);
            byte[] bytes = marker.Width == 1 ? Encoding.ASCII.GetBytes(replacement)
                                            : Encoding.Unicode.GetBytes(replacement);
            Require(bytes.Length == prefix.Length * marker.Width, "Replacement changed a literal's size.");
            Buffer.BlockCopy(bytes, 0, data, marker.Offset, bytes.Length);
        }
        Verify(data);
        int checksumOffset = optional + 64;
        if (U32(input, checksumOffset) != 0)
            Buffer.BlockCopy(BitConverter.GetBytes(Checksum(data, checksumOffset)), 0, data, checksumOffset, 4);
        return data;
    }
}
}
'@
}

function Protect-QuadDeckRelease {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$StagingDirectory)
    $files = @(Get-ChildItem -LiteralPath $StagingDirectory -File -Recurse)
    foreach ($file in $files) {
        if ($file.Extension -notin @('.exe', '.dll')) { continue }
        $original = [IO.File]::ReadAllBytes($file.FullName)
        try { $clean = [QuadDeck.ReleasePrivacy]::Sanitize($original, $file.Name) }
        catch { throw "Release privacy check failed for $($file.Name): $($_.Exception.Message)" }
        [IO.File]::WriteAllBytes($file.FullName, $clean)
    }
    Assert-QuadDeckReleasePrivacy -Directory $StagingDirectory
}

function Assert-QuadDeckReleasePrivacy {
    [CmdletBinding(DefaultParameterSetName = 'Directory')]
    param(
        [Parameter(Mandatory, ParameterSetName = 'Directory')][string]$Directory,
        [Parameter(Mandatory, ParameterSetName = 'Archive')][string]$Archive
    )
    if ($PSCmdlet.ParameterSetName -eq 'Directory') {
        foreach ($file in Get-ChildItem -LiteralPath $Directory -File -Recurse) {
            if ($file.Extension -in @('.pdb', '.log', '.qauth', '.qconfig', '.qdeck', '.qstyle')) {
                throw "Private state or symbols must not enter a release: $($file.Name)"
            }
            try { [QuadDeck.ReleasePrivacy]::Verify([IO.File]::ReadAllBytes($file.FullName)) }
            catch { throw "Release privacy check failed for $($file.Name): $($_.Exception.Message)" }
        }
        return
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($Archive))
    try {
        foreach ($entry in $zip.Entries) {
            if (-not $entry.Name) { continue }
            if ([IO.Path]::GetExtension($entry.Name) -in @('.pdb', '.log', '.qauth', '.qconfig', '.qdeck', '.qstyle')) {
                throw "Private state or symbols must not enter a release: $($entry.Name)"
            }
            $stream = $entry.Open()
            $buffer = New-Object IO.MemoryStream
            try {
                $stream.CopyTo($buffer)
                [QuadDeck.ReleasePrivacy]::Verify($buffer.ToArray())
            }
            catch { throw "Release privacy check failed for $($entry.Name): $($_.Exception.Message)" }
            finally { $stream.Dispose(); $buffer.Dispose() }
        }
    }
    finally { $zip.Dispose() }
}

Export-ModuleMember -Function Protect-QuadDeckRelease, Assert-QuadDeckReleasePrivacy
