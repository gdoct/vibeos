namespace DiskUtil.Core.Filesystems.VibeFs.v1_0;

public enum VibeFsNodeType
{
    Unknown = 0,
    File = 1,
    Directory = 2,
    Symlink = 3
}

public sealed record VibeFsNode(
    uint Inode,
    string Name,
    string FullPath,
    VibeFsNodeType Type,
    ulong Size);
