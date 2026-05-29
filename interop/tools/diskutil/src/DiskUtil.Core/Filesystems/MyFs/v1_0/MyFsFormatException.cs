namespace DiskUtil.Core.Filesystems.MyFs.v1_0;

public sealed class MyFsFormatException : Exception
{
    public MyFsFormatException(string message) : base(message)
    {
    }
}
