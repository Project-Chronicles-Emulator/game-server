using System.Buffers.Binary;

// Presents the standard 221-byte UE footer without changing the installed PAK.
sealed class FooterStream : Stream, ICloneable
{
    readonly FileStream source;
    readonly byte[] footer;
    readonly long footerStart;
    long position;
    public FooterStream(string path)
    {
        source = File.OpenRead(path);
        footerStart = source.Length - 229;
        source.Position = footerStart;
        var original = new byte[229];
        source.ReadExactly(original);
        if (BinaryPrimitives.ReadUInt32LittleEndian(original.AsSpan(17)) != 0x5835F00D
            || BinaryPrimitives.ReadInt32LittleEndian(original.AsSpan(29)) != 12)
            throw new InvalidDataException("Unexpected Chronicles PAK footer");
        footer = new byte[221];
        original.AsSpan(0, 21).CopyTo(footer);
        original.AsSpan(29).CopyTo(footer.AsSpan(21));
        BinaryPrimitives.WriteUInt32LittleEndian(footer.AsSpan(17), 0x5A6F12E1);
    }
    public override int Read(byte[] buffer, int offset, int count)
    {
        count = (int)Math.Min(count, Length - position);
        if (count <= 0) return 0;
        var n = 0;
        if (position < footerStart)
        {
            source.Position = position;
            n = source.Read(buffer, offset, (int)Math.Min(count, footerStart-position));
            position += n;
        }
        if (n < count && position >= footerStart)
        {
            footer.AsSpan((int)(position-footerStart),count-n).CopyTo(buffer.AsSpan(offset+n));
            position += count-n;
            n = count;
        }
        return n;
    }
    public object Clone() => new FooterStream(source.Name) { Position=position };
    public override long Seek(long offset, SeekOrigin origin) => Position = origin switch
    { SeekOrigin.Begin => offset, SeekOrigin.Current => position+offset, SeekOrigin.End => Length+offset, _ => throw new ArgumentOutOfRangeException() };
    public override bool CanRead => true;
    public override bool CanSeek => true;
    public override bool CanWrite => false;
    public override long Length => source.Length-8;
    public override long Position { get => position; set => position=value; }
    public override void Flush() { }
    public override void SetLength(long value) => throw new NotSupportedException();
    public override void Write(byte[] buffer,int offset,int count) => throw new NotSupportedException();
    protected override void Dispose(bool disposing) { if(disposing) source.Dispose(); base.Dispose(disposing); }
}
