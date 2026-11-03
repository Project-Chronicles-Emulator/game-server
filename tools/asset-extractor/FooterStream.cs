using System.Buffers.Binary;

// Presents the standard 221-byte UE footer without changing the installed PAK.
sealed class FooterStream : Stream, ICloneable
{
    private const int StoredFooterBytes = 229;
    private const int NormalizedFooterBytes = 221;
    private const int MagicOffset = 17;
    private const int StoredVersionOffset = 29;
    private const int NormalizedVersionOffset = 21;
    private const int SupportedPakVersion = 12;
    private const uint ChroniclesPakMagic = 0x5835F00D;
    private const uint UnrealPakMagic = 0x5A6F12E1;

    readonly FileStream sourceStream;
    readonly byte[] normalizedFooter;
    readonly long footerOffset;
    long currentPosition;

    public FooterStream(string path)
    {
        sourceStream = File.OpenRead(path);
        try
        {
            if (sourceStream.Length < StoredFooterBytes)
            {
                throw new InvalidDataException("Chronicles PAK is shorter than its footer.");
            }
            footerOffset = sourceStream.Length - StoredFooterBytes;
            sourceStream.Position = footerOffset;
            var original = new byte[StoredFooterBytes];
            sourceStream.ReadExactly(original);
            if (BinaryPrimitives.ReadUInt32LittleEndian(original.AsSpan(MagicOffset)) != ChroniclesPakMagic
                || BinaryPrimitives.ReadInt32LittleEndian(original.AsSpan(StoredVersionOffset)) != SupportedPakVersion)
            {
                throw new InvalidDataException("Unexpected Chronicles PAK footer");
            }

            normalizedFooter = new byte[NormalizedFooterBytes];
            original.AsSpan(0, NormalizedVersionOffset).CopyTo(normalizedFooter);
            original.AsSpan(StoredVersionOffset).CopyTo(normalizedFooter.AsSpan(NormalizedVersionOffset));
            BinaryPrimitives.WriteUInt32LittleEndian(normalizedFooter.AsSpan(MagicOffset), UnrealPakMagic);
        }
        catch
        {
            sourceStream.Dispose();
            throw;
        }
    }

    public override int Read(byte[] buffer, int offset, int count)
    {
        count = (int)Math.Min(count, Length - currentPosition);
        if (count <= 0)
        {
            return 0;
        }

        var bytesRead = 0;
        if (currentPosition < footerOffset)
        {
            sourceStream.Position = currentPosition;
            bytesRead = sourceStream.Read(
                buffer, offset, (int)Math.Min(count, footerOffset - currentPosition));
            currentPosition += bytesRead;
        }
        if (bytesRead < count && currentPosition >= footerOffset)
        {
            normalizedFooter.AsSpan((int)(currentPosition - footerOffset), count - bytesRead)
                .CopyTo(buffer.AsSpan(offset + bytesRead));
            currentPosition += count - bytesRead;
            bytesRead = count;
        }
        return bytesRead;
    }

    public object Clone()
    {
        return new FooterStream(sourceStream.Name) { Position = currentPosition };
    }

    public override long Seek(long offset, SeekOrigin origin)
    {
        Position = origin switch
        {
            SeekOrigin.Begin => offset,
            SeekOrigin.Current => currentPosition + offset,
            SeekOrigin.End => Length + offset,
            _ => throw new ArgumentOutOfRangeException(nameof(origin))
        };
        return Position;
    }

    public override bool CanRead => true;

    public override bool CanSeek => true;

    public override bool CanWrite => false;

    public override long Length => sourceStream.Length - (StoredFooterBytes - NormalizedFooterBytes);

    public override long Position
    {
        get => currentPosition;
        set
        {
            if (value < 0)
            {
                throw new ArgumentOutOfRangeException(nameof(value));
            }
            currentPosition = value;
        }
    }

    public override void Flush()
    {
    }

    public override void SetLength(long value)
    {
        throw new NotSupportedException();
    }

    public override void Write(byte[] buffer, int offset, int count)
    {
        throw new NotSupportedException();
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            sourceStream.Dispose();
        }

        base.Dispose(disposing);
    }
}
