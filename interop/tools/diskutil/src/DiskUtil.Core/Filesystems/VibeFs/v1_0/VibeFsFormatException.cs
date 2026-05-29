namespace DiskUtil.Core.Filesystems.VibeFs.v1_0;

public sealed class VibeFsFormatException : Exception
{
    public VibeFsFormatException(string message) : base(message)
    {
    }
}
