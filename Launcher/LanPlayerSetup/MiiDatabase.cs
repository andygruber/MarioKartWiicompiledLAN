using System.Buffers.Binary;
using System.Text;

namespace LanPlayerSetup;

/// <summary>
/// The Wii's Mii database, <c>/shared2/menu/FaceLib/RFL_DB.dat</c>. Mario Kart Wii shows other
/// players the name of the Mii a license uses; with no database the game only offers guest Miis,
/// which it always prints as "Player" (MiiNameMsgPrinter). This class keeps one Mii of our own in
/// the database, recognised by its creator name, and renames it in place so a license that uses it
/// keeps working.
///
/// Layout (RFL, the Mii library the game links): "RNOD", 100 slots of 74-byte Mii records, a
/// slot bitmap at 0x1CEC, the "RNHD" hidden-database header at 0x1D00 and a CRC-16/XMODEM over
/// everything before 0x1F1DE.
/// </summary>
public sealed class MiiDatabase
{
    public const int FileSize = 0xBE6C0;
    public const int SlotCount = 100;
    public const int RecordSize = 0x4A;
    public const int NameLength = 10;
    public const string DefaultName = "Player";
    public const string OwnCreator = "LAN Player";
    public static readonly string RelativePath = Path.Combine("shared2", "menu", "FaceLib", "RFL_DB.dat");

    private const int SlotsOffset = 4;
    private const int SlotBitmapOffset = 0x1CEC;
    private const int HiddenHeaderOffset = 0x1D00;
    private const int CrcOffset = 0x1F1DE;
    private static readonly byte[] Magic = "RNOD"u8.ToArray();
    private static readonly byte[] HiddenMagic = "RNHD"u8.ToArray();

    public static readonly string[] FavoriteColors =
    [
        "Red", "Orange", "Yellow", "Light green", "Green", "Blue",
        "Light blue", "Pink", "Purple", "Brown", "White", "Black",
    ];

    private readonly byte[] _data;

    private MiiDatabase(byte[] data, bool existed)
    {
        _data = data;
        Existed = existed;
    }

    /// <summary>Whether the database was read from disk (false: created empty).</summary>
    public bool Existed { get; }

    public static MiiDatabase Load(string path)
    {
        if (!File.Exists(path)) return CreateEmpty();
        var data = File.ReadAllBytes(path);
        if (data.Length < CrcOffset + 2 || !data.AsSpan(0, 4).SequenceEqual(Magic))
            throw new InvalidDataException($"{path} is not a Mii database.");
        if (BinaryPrimitives.ReadUInt16BigEndian(data.AsSpan(CrcOffset)) != Crc16(data.AsSpan(0, CrcOffset)))
            throw new InvalidDataException($"{path} is damaged (checksum mismatch).");
        return new MiiDatabase(data, existed: true);
    }

    public static MiiDatabase CreateEmpty()
    {
        var data = new byte[FileSize];
        Magic.CopyTo(data, 0);
        HiddenMagic.CopyTo(data, HiddenHeaderOffset);
        BinaryPrimitives.WriteUInt32BigEndian(data.AsSpan(HiddenHeaderOffset + 4), 0xFFFFFFFF);
        var database = new MiiDatabase(data, existed: false);
        database.UpdateChecksum();
        return database;
    }

    public IEnumerable<(int Slot, string Name, string Creator)> Miis()
    {
        for (var slot = 0; slot < SlotCount; slot++)
        {
            var record = Record(slot);
            if (record.IndexOfAnyExcept((byte)0) >= 0)
                yield return (slot, ReadName(record[2..]), ReadName(record[0x36..]));
        }
    }

    /// <summary>The Mii this tool manages, if the database has one.</summary>
    public PlayerMii? FindOwn()
    {
        foreach (var (slot, name, creator) in Miis())
        {
            if (creator != OwnCreator) continue;
            var record = Record(slot);
            var header = BinaryPrimitives.ReadUInt16BigEndian(record);
            return new PlayerMii(name, IsGirl: (header & 0x4000) != 0, FavoriteColor: (header >> 1) & 0xF);
        }
        return null;
    }

    /// <summary>Creates or updates our Mii. Its slot and Mii ID never change once created.</summary>
    public void SetOwn(PlayerMii mii)
    {
        ValidateName(mii.Name);
        if (mii.FavoriteColor is < 0 or > 11) throw new ArgumentException("Favorite color must be 0-11.");

        int slot = -1;
        byte[] createId = new byte[8];
        foreach (var (candidate, _, creator) in Miis())
        {
            if (creator != OwnCreator) continue;
            slot = candidate;
            Record(candidate).Slice(0x18, 8).CopyTo(createId);
            break;
        }
        if (slot < 0)
        {
            slot = FirstFreeSlot() ?? throw new InvalidOperationException("The Mii database is full (100 Miis).");
            createId = NewCreateId();
            _data[SlotBitmapOffset + slot / 8] |= (byte)(0x80 >> (slot % 8));
        }

        EncodeRecord(mii, createId).CopyTo(Record(slot));
        UpdateChecksum();
    }

    public void Save(string path)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        // Keep the user's own database (e.g. copied from Dolphin) the first time we change it.
        var backup = path + ".bak";
        if (File.Exists(path) && !File.Exists(backup) && FindOwnIn(path) is null)
            File.Copy(path, backup);
        var temporary = path + ".tmp";
        File.WriteAllBytes(temporary, _data);
        File.Move(temporary, path, overwrite: true);
    }

    public static void ValidateName(string name)
    {
        if (string.IsNullOrWhiteSpace(name))
            throw new ArgumentException("The name must not be empty.");
        if (name.Length > NameLength)
            throw new ArgumentException($"The name can have at most {NameLength} characters.");
        foreach (var c in name)
        {
            if (char.IsSurrogate(c) || char.IsControl(c))
                throw new ArgumentException($"The name cannot contain '{c}'.");
        }
    }

    internal static ushort Crc16(ReadOnlySpan<byte> data)
    {
        ushort crc = 0;
        foreach (var b in data)
        {
            crc ^= (ushort)(b << 8);
            for (var i = 0; i < 8; i++)
                crc = (crc & 0x8000) != 0 ? (ushort)((crc << 1) ^ 0x1021) : (ushort)(crc << 1);
        }
        return crc;
    }

    internal ReadOnlySpan<byte> Bytes => _data;

    private static PlayerMii? FindOwnIn(string path)
    {
        try { return Load(path).FindOwn(); }
        catch (InvalidDataException) { return null; }
    }

    private Span<byte> Record(int slot) => _data.AsSpan(SlotsOffset + slot * RecordSize, RecordSize);

    private int? FirstFreeSlot()
    {
        for (var slot = 0; slot < SlotCount; slot++)
        {
            if (Record(slot).IndexOfAnyExcept((byte)0) < 0) return slot;
        }
        return null;
    }

    private void UpdateChecksum() =>
        BinaryPrimitives.WriteUInt16BigEndian(_data.AsSpan(CrcOffset), Crc16(_data.AsSpan(0, CrcOffset)));

    private static string ReadName(ReadOnlySpan<byte> field)
    {
        var text = Encoding.BigEndianUnicode.GetString(field[..(NameLength * 2)]);
        var end = text.IndexOf('\0');
        return end < 0 ? text : text[..end];
    }

    /// <summary>
    /// Mii ID: the top three bits 100 mark an ordinary Mii (RFL rejects bit 29), the rest is the
    /// creation time in 4-second steps since 2006, as the Mii Channel writes it. The console ID half
    /// only needs to be non-zero.
    /// </summary>
    private static byte[] NewCreateId()
    {
        var id = new byte[8];
        var ticks = (uint)((DateTimeOffset.UtcNow - new DateTimeOffset(2006, 1, 1, 0, 0, 0, TimeSpan.Zero)).TotalSeconds / 4);
        BinaryPrimitives.WriteUInt32BigEndian(id, 0x80000000u | (ticks & 0x1FFFFFFFu));
        var console = (uint)Random.Shared.Next(1, int.MaxValue);
        BinaryPrimitives.WriteUInt32BigEndian(id.AsSpan(4), console);
        return id;
    }

    /// <summary>The Mii Channel's default male or female face with the chosen name and color.</summary>
    private static byte[] EncodeRecord(PlayerMii mii, byte[] createId)
    {
        var record = new byte[RecordSize];
        var girl = mii.IsGirl ? 1u : 0u;
        // invalid:1 girl:1 month:4 day:5 favoriteColor:4 favorite:1
        Put16(record, 0x00, Bits((0, 1), (girl, 1), (0, 4), (0, 5), ((uint)mii.FavoriteColor, 4), (0, 1)));
        WriteName(record.AsSpan(0x02), mii.Name);
        record[0x16] = 64; // height
        record[0x17] = 64; // weight
        createId.CopyTo(record, 0x18);
        // faceShape:3 skin:3 facialFeature:4 unknown:3 mingleOff:1 unknown:1 downloaded:1
        Put16(record, 0x20, 0);
        // hairType:7 hairColor:3 hairPart:1 unknown:5
        Put16(record, 0x22, Bits((mii.IsGirl ? 12u : 33u, 7), (1, 3), (0, 1), (0, 5)));
        // eyebrowType:5 unknown:1 rotation:4 unknown:6 color:3 size:4 vertical:5 horizontal:4
        Put32(record, 0x24, Bits((mii.IsGirl ? 0u : 6u, 5), (0, 1), (6, 4), (0, 6), (1, 3), (4, 4), (10, 5), (2, 4)));
        // eyeType:6 unknown:2 rotation:3 vertical:5 color:3 unknown:1 size:3 horizontal:4 unknown:5
        Put32(record, 0x28, Bits((mii.IsGirl ? 4u : 2u, 6), (0, 2), (mii.IsGirl ? 3u : 4u, 3), (12, 5), (0, 3), (0, 1), (4, 3), (2, 4), (0, 5)));
        // noseType:4 size:4 vertical:5 unknown:3
        Put16(record, 0x2C, Bits((1, 4), (4, 4), (9, 5), (0, 3)));
        // lipType:5 color:2 size:4 vertical:5
        Put16(record, 0x2E, Bits((23, 5), (0, 2), (4, 4), (13, 5)));
        // glassesType:4 color:3 unknown:1 size:3 vertical:5
        Put16(record, 0x30, Bits((0, 4), (0, 3), (0, 1), (4, 3), (10, 5)));
        // mustache:2 beard:2 color:3 size:4 vertical:5
        Put16(record, 0x32, Bits((0, 2), (0, 2), (0, 3), (4, 4), (10, 5)));
        // mole:1 size:4 vertical:5 horizontal:5 unknown:1
        Put16(record, 0x34, Bits((0, 1), (4, 4), (20, 5), (2, 5), (0, 1)));
        WriteName(record.AsSpan(0x36), OwnCreator);
        return record;
    }

    private static uint Bits(params (uint Value, int Width)[] fields)
    {
        uint value = 0;
        foreach (var (field, width) in fields)
            value = (value << width) | (field & ((1u << width) - 1));
        return value;
    }

    private static void Put16(byte[] record, int offset, uint value) =>
        BinaryPrimitives.WriteUInt16BigEndian(record.AsSpan(offset), (ushort)value);

    private static void Put32(byte[] record, int offset, uint value) =>
        BinaryPrimitives.WriteUInt32BigEndian(record.AsSpan(offset), value);

    private static void WriteName(Span<byte> field, string name)
    {
        field[..(NameLength * 2)].Clear();
        Encoding.BigEndianUnicode.GetBytes(name).CopyTo(field);
    }
}

public sealed record PlayerMii(string Name, bool IsGirl = false, int FavoriteColor = 0);
