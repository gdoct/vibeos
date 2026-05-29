namespace DiskUtil.Core.Filesystems.MyFs.v1_0;

public enum MyFsNodeType
{
    Unknown = 0,
    File = 1,
    Directory = 2
}

public sealed record MyFsNode(
    uint Inode,
    string Name,
    string FullPath,
    MyFsNodeType Type,
    ulong Size);
