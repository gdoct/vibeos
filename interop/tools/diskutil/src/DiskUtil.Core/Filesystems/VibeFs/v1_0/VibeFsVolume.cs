using System.Buffers.Binary;
using System.Text;

namespace DiskUtil.Core.Filesystems.VibeFs.v1_0;

public sealed class VibeFsVolume : IDisposable
{
    private const uint FsMagic = 0x4D594653;
    private const uint FsVersion = 2;
    private const uint FsBlockSize = 4096;
    private const int FsNameMax = 60;
    private const int DirentHeaderSize = 8;
    private const int InodeSize = 128;
    private const int InodesPerBlock = (int)(FsBlockSize / InodeSize);
    private const int Ndirect = 13;
    private const int PointersPerBlock = (int)(FsBlockSize / sizeof(uint));

    private readonly Stream _stream;
    private readonly bool _ownsStream;
    private readonly bool _canWrite;
    private readonly Superblock _superblock;
    private readonly byte[] _inodeBitmap;
    private readonly byte[] _dataBitmap;
    private readonly Dictionary<uint, uint[]> _pointerBlockCache = new();

    public VibeFsVolume(Stream stream, bool ownsStream = false)
    {
        if (stream is null)
        {
            throw new ArgumentNullException(nameof(stream));
        }

        if (!stream.CanRead || !stream.CanSeek)
        {
            throw new ArgumentException("Stream must support read and seek.", nameof(stream));
        }

        _stream = stream;
        _ownsStream = ownsStream;
        _canWrite = stream.CanWrite;
        _superblock = ReadSuperblock();
        _inodeBitmap = ReadBitmap(_superblock.InodeBitmapBlock, _superblock.InodeBitmapBlocks);
        _dataBitmap = ReadBitmap(_superblock.DataBitmapBlock, _superblock.DataBitmapBlocks);
    }

    public static VibeFsVolume Open(string imagePath)
    {
        if (string.IsNullOrWhiteSpace(imagePath))
        {
            throw new ArgumentException("Image path is required.", nameof(imagePath));
        }

        return new VibeFsVolume(File.Open(imagePath, FileMode.Open, FileAccess.Read, FileShare.Read), ownsStream: true);
    }

    public static VibeFsVolume OpenReadWrite(string imagePath)
    {
        if (string.IsNullOrWhiteSpace(imagePath))
        {
            throw new ArgumentException("Image path is required.", nameof(imagePath));
        }

        return new VibeFsVolume(File.Open(imagePath, FileMode.Open, FileAccess.ReadWrite, FileShare.Read), ownsStream: true);
    }

    public static void Format(Stream stream, uint totalBlocks)
    {
        if (stream is null)
        {
            throw new ArgumentNullException(nameof(stream));
        }

        if (!stream.CanRead || !stream.CanSeek || !stream.CanWrite)
        {
            throw new ArgumentException("Stream must support read, seek, and write.", nameof(stream));
        }

        if (totalBlocks < 16)
        {
            throw new ArgumentOutOfRangeException(nameof(totalBlocks), "VibeFS image must be at least 16 blocks.");
        }

        stream.SetLength((long)totalBlocks * FsBlockSize);

        var inodeCount = totalBlocks / 4;
        if (inodeCount < 16)
        {
            inodeCount = 16;
        }

        if (inodeCount > 32768)
        {
            inodeCount = 32768;
        }

        inodeCount = (inodeCount + 31u) & ~31u;

        var inodeBitmapBlocks = 1u;
        var inodeTableBlocks = inodeCount / (uint)InodesPerBlock;
        var after = totalBlocks - 1u - inodeBitmapBlocks - inodeTableBlocks;
        var dataBitmapBlocks = (after + (FsBlockSize * 8u) - 1u) / (FsBlockSize * 8u);
        if (dataBitmapBlocks == 0)
        {
            dataBitmapBlocks = 1;
        }

        var inodeBitmapBlock = 1u;
        var dataBitmapBlock = inodeBitmapBlock + inodeBitmapBlocks;
        var inodeTableBlock = dataBitmapBlock + dataBitmapBlocks;
        var dataStartBlock = inodeTableBlock + inodeTableBlocks;
        var dataBlocks = totalBlocks - dataStartBlock;

        var superblock = new byte[FsBlockSize];
        BinaryPrimitives.WriteUInt32LittleEndian(superblock.AsSpan(0, 4), FsMagic);
        BinaryPrimitives.WriteUInt32LittleEndian(superblock.AsSpan(4, 4), FsVersion);
        BinaryPrimitives.WriteUInt32LittleEndian(superblock.AsSpan(8, 4), FsBlockSize);
        BinaryPrimitives.WriteUInt32LittleEndian(superblock.AsSpan(12, 4), totalBlocks);
        BinaryPrimitives.WriteUInt32LittleEndian(superblock.AsSpan(16, 4), inodeCount);
        BinaryPrimitives.WriteUInt32LittleEndian(superblock.AsSpan(20, 4), inodeBitmapBlock);
        BinaryPrimitives.WriteUInt32LittleEndian(superblock.AsSpan(24, 4), inodeBitmapBlocks);
        BinaryPrimitives.WriteUInt32LittleEndian(superblock.AsSpan(28, 4), dataBitmapBlock);
        BinaryPrimitives.WriteUInt32LittleEndian(superblock.AsSpan(32, 4), dataBitmapBlocks);
        BinaryPrimitives.WriteUInt32LittleEndian(superblock.AsSpan(36, 4), inodeTableBlock);
        BinaryPrimitives.WriteUInt32LittleEndian(superblock.AsSpan(40, 4), inodeTableBlocks);
        BinaryPrimitives.WriteUInt32LittleEndian(superblock.AsSpan(44, 4), dataStartBlock);
        BinaryPrimitives.WriteUInt32LittleEndian(superblock.AsSpan(48, 4), dataBlocks);
        BinaryPrimitives.WriteUInt32LittleEndian(superblock.AsSpan(52, 4), 1);

        WriteBlockStatic(stream, 0, superblock);

        var zeroBlock = new byte[FsBlockSize];
        for (uint i = 0; i < inodeBitmapBlocks; i++)
        {
            WriteBlockStatic(stream, inodeBitmapBlock + i, zeroBlock);
        }

        for (uint i = 0; i < dataBitmapBlocks; i++)
        {
            WriteBlockStatic(stream, dataBitmapBlock + i, zeroBlock);
        }

        for (uint i = 0; i < inodeTableBlocks; i++)
        {
            WriteBlockStatic(stream, inodeTableBlock + i, zeroBlock);
        }

        var inodeBitmap = new byte[FsBlockSize];
        SetBit(inodeBitmap, 0);
        SetBit(inodeBitmap, 1);
        WriteBlockStatic(stream, inodeBitmapBlock, inodeBitmap);

        var dataBitmap = new byte[dataBitmapBlocks * FsBlockSize];
        SetBit(dataBitmap, 0);
        for (var i = 0; i < dataBitmapBlocks; i++)
        {
            var start = i * (int)FsBlockSize;
            WriteBlockStatic(stream, dataBitmapBlock + (uint)i, dataBitmap.AsSpan(start, (int)FsBlockSize).ToArray());
        }

        var rootInodeBlock = new byte[FsBlockSize];
        var rootInodeSpan = rootInodeBlock.AsSpan(InodeSize, InodeSize);
        BinaryPrimitives.WriteUInt16LittleEndian(rootInodeSpan.Slice(0, 2), (ushort)VibeFsNodeType.Directory);
        BinaryPrimitives.WriteUInt16LittleEndian(rootInodeSpan.Slice(2, 2), 2);
        BinaryPrimitives.WriteUInt64LittleEndian(rootInodeSpan.Slice(4, 8), FsBlockSize);
        BinaryPrimitives.WriteUInt32LittleEndian(rootInodeSpan.Slice(20, 4), dataStartBlock);
        WriteBlockStatic(stream, inodeTableBlock, rootInodeBlock);

        var rootDirectoryBlock = new byte[FsBlockSize];
        var offset = 0;
        WriteDirectoryEntry(rootDirectoryBlock, ref offset, 1, ".", (byte)VibeFsNodeType.Directory, 12);
        WriteDirectoryEntry(rootDirectoryBlock, ref offset, 1, "..", (byte)VibeFsNodeType.Directory, 12);
        WriteDirectoryEntry(rootDirectoryBlock, ref offset, 0, string.Empty, 0, checked((ushort)(FsBlockSize - offset)));
        WriteBlockStatic(stream, dataStartBlock, rootDirectoryBlock);
        stream.Flush();
    }

    public IReadOnlyList<VibeFsNode> ListDirectory(string path)
    {
        var normalizedPath = NormalizeAbsolutePath(path);
        var resolved = ResolvePath(normalizedPath);
        if (resolved.Inode.Type != (ushort)VibeFsNodeType.Directory)
        {
            throw new InvalidOperationException($"Path '{normalizedPath}' is not a directory.");
        }

        var entries = new List<VibeFsNode>();
        foreach (var dirEntry in EnumerateDirectoryEntries(resolved.Inode))
        {
            if (dirEntry.Inode == 0)
            {
                continue;
            }

            if (dirEntry.Name is "." or "..")
            {
                continue;
            }

            var childInode = ReadInode(dirEntry.Inode);
            var fullPath = JoinPath(normalizedPath, dirEntry.Name);
            entries.Add(new VibeFsNode(
                dirEntry.Inode,
                dirEntry.Name,
                fullPath,
                MapType(childInode.Type),
                childInode.Size));
        }

        return entries
            .OrderByDescending(x => x.Type == VibeFsNodeType.Directory)
            .ThenBy(x => x.Name, StringComparer.OrdinalIgnoreCase)
            .ToArray();
    }

    public VibeFsNode GetNode(string path)
    {
        var normalizedPath = NormalizeAbsolutePath(path);
        var resolved = ResolvePath(normalizedPath);
        var name = normalizedPath == "/"
            ? "/"
            : normalizedPath.Split('/', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).Last();

        return new VibeFsNode(
            resolved.InodeNumber,
            name,
            normalizedPath,
            MapType(resolved.Inode.Type),
            resolved.Inode.Size);
    }

    public byte[] ReadFileBytes(string path, ulong? maxBytes = null)
    {
        var normalizedPath = NormalizeAbsolutePath(path);
        var resolved = ResolvePath(normalizedPath);
        if (resolved.Inode.Type != (ushort)VibeFsNodeType.File)
        {
            throw new InvalidOperationException($"Path '{normalizedPath}' is not a file.");
        }

        var fileSize = resolved.Inode.Size;
        var targetLength = maxBytes.HasValue ? Math.Min(fileSize, maxBytes.Value) : fileSize;
        if (targetLength > int.MaxValue)
        {
            throw new InvalidOperationException(
                "File is too large to read into memory. Pass a maxBytes value for partial reads.");
        }

        var output = new byte[targetLength];
        var blockBuffer = new byte[FsBlockSize];

        ulong outputOffset = 0;
        while (outputOffset < targetLength)
        {
            var fileBlockIndex = (uint)(outputOffset / FsBlockSize);
            var blockOffset = (int)(outputOffset % FsBlockSize);
            var remaining = (int)Math.Min((ulong)FsBlockSize - (ulong)blockOffset, targetLength - outputOffset);

            var diskBlock = MapFileBlock(resolved.Inode, fileBlockIndex);
            if (diskBlock == 0)
            {
                Array.Clear(output, (int)outputOffset, remaining);
            }
            else
            {
                ReadBlock(diskBlock, blockBuffer);
                Buffer.BlockCopy(blockBuffer, blockOffset, output, (int)outputOffset, remaining);
            }

            outputOffset += (ulong)remaining;
        }

        return output;
    }

    public void WriteFile(string path, byte[] content)
    {
        if (content is null)
        {
            throw new ArgumentNullException(nameof(content));
        }

        EnsureWritable();

        var normalizedPath = NormalizeAbsolutePath(path);
        var existing = TryResolvePath(normalizedPath);

        uint inodeNumber;
        Inode inode;

        if (existing is not null)
        {
            if (existing.Value.Inode.Type != (ushort)VibeFsNodeType.File)
            {
                throw new InvalidOperationException($"Path '{normalizedPath}' is not a regular file.");
            }

            inodeNumber = existing.Value.InodeNumber;
            inode = existing.Value.Inode;
            FreeInodeBlocks(inode);
        }
        else
        {
            var parent = ResolveParentPath(normalizedPath);
            if (FindDirectoryEntry(parent.ParentInode, parent.Name) is not null)
            {
                throw new InvalidOperationException($"Path '{normalizedPath}' already exists.");
            }

            inodeNumber = AllocateInode();
            inode = Inode.Create((ushort)VibeFsNodeType.File, 1);
            AddDirectoryEntry(parent.ParentInodeNumber, parent.ParentInode, parent.Name, inodeNumber, (byte)VibeFsNodeType.File);
        }

        var blockBuffer = new byte[FsBlockSize];
        var contentOffset = 0;
        var blockIndex = 0u;
        while (contentOffset < content.Length)
        {
            Array.Clear(blockBuffer);
            var bytesToCopy = Math.Min((int)FsBlockSize, content.Length - contentOffset);
            Buffer.BlockCopy(content, contentOffset, blockBuffer, 0, bytesToCopy);

            var diskBlock = MapFileBlock(inode, blockIndex, allocate: true);
            WriteBlock(diskBlock, blockBuffer);

            contentOffset += bytesToCopy;
            blockIndex++;
        }

        inode.Size = (ulong)content.Length;
        WriteInode(inodeNumber, inode);
        FlushBitmaps();
    }

    public void CreateDirectory(string path)
    {
        EnsureWritable();

        var normalizedPath = NormalizeAbsolutePath(path);
        if (normalizedPath == "/")
        {
            throw new InvalidOperationException("Cannot create root directory.");
        }

        if (TryResolvePath(normalizedPath) is not null)
        {
            throw new InvalidOperationException($"Path '{normalizedPath}' already exists.");
        }

        var parent = ResolveParentPath(normalizedPath);
        var inodeNumber = AllocateInode();
        var inode = Inode.Create((ushort)VibeFsNodeType.Directory, 2);

        var firstBlock = AllocateDataBlock();
        inode.Direct[0] = firstBlock;
        inode.Size = FsBlockSize;

        var block = new byte[FsBlockSize];
        var offset = 0;
        WriteDirectoryEntry(block, ref offset, inodeNumber, ".", (byte)VibeFsNodeType.Directory, 12);
        WriteDirectoryEntry(block, ref offset, parent.ParentInodeNumber, "..", (byte)VibeFsNodeType.Directory, 12);
        WriteDirectoryEntry(block, ref offset, 0, string.Empty, 0, checked((ushort)(FsBlockSize - offset)));
        WriteBlock(firstBlock, block);

        WriteInode(inodeNumber, inode);
        AddDirectoryEntry(parent.ParentInodeNumber, parent.ParentInode, parent.Name, inodeNumber, (byte)VibeFsNodeType.Directory);

        parent.ParentInode.Links++;
        WriteInode(parent.ParentInodeNumber, parent.ParentInode);
        FlushBitmaps();
    }

    public void Delete(string path)
    {
        EnsureWritable();

        var normalizedPath = NormalizeAbsolutePath(path);
        if (normalizedPath == "/")
        {
            throw new InvalidOperationException("Cannot delete root directory.");
        }

        var parent = ResolveParentPath(normalizedPath);
        var entry = FindDirectoryEntry(parent.ParentInode, parent.Name)
            ?? throw new FileNotFoundException($"Path '{normalizedPath}' was not found.", normalizedPath);

        var inode = ReadInode(entry.Inode);
        if (inode.Type == (ushort)VibeFsNodeType.Directory && !IsDirectoryEmpty(inode))
        {
            throw new InvalidOperationException("Directory is not empty.");
        }

        RemoveDirectoryEntry(parent.ParentInodeNumber, parent.ParentInode, parent.Name);

        if (inode.Type == (ushort)VibeFsNodeType.Directory && parent.ParentInode.Links > 0)
        {
            parent.ParentInode.Links--;
            WriteInode(parent.ParentInodeNumber, parent.ParentInode);
        }

        FreeInodeBlocks(inode);
        WriteInode(entry.Inode, Inode.Create(0, 0));
        ClearBit(_inodeBitmap, (int)entry.Inode);

        FlushBitmaps();
    }

    public void CopyFile(string sourcePath, string destinationPath)
    {
        var source = GetNode(sourcePath);
        if (source.Type != VibeFsNodeType.File)
        {
            throw new InvalidOperationException("Only regular files can be copied.");
        }

        var bytes = ReadFileBytes(source.FullPath);
        WriteFile(destinationPath, bytes);
    }

    public void Dispose()
    {
        if (_ownsStream)
        {
            _stream.Dispose();
        }
    }

    private Superblock ReadSuperblock()
    {
        var block = new byte[FsBlockSize];
        ReadBlock(0, block);

        var superblock = new Superblock(
            Magic: BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(0, 4)),
            Version: BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(4, 4)),
            BlockSize: BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(8, 4)),
            TotalBlocks: BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(12, 4)),
            InodeCount: BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(16, 4)),
            InodeBitmapBlock: BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(20, 4)),
            InodeBitmapBlocks: BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(24, 4)),
            DataBitmapBlock: BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(28, 4)),
            DataBitmapBlocks: BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(32, 4)),
            InodeTableBlock: BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(36, 4)),
            InodeTableBlocks: BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(40, 4)),
            DataStartBlock: BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(44, 4)),
            DataBlocks: BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(48, 4)),
            RootInode: BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(52, 4)));

        if (superblock.Magic != FsMagic)
        {
            throw new VibeFsFormatException($"Invalid magic 0x{superblock.Magic:X8}; expected 0x{FsMagic:X8}.");
        }

        if (superblock.Version != FsVersion)
        {
            throw new VibeFsFormatException($"Unsupported VibeFS version {superblock.Version}; expected {FsVersion}.");
        }

        if (superblock.BlockSize != FsBlockSize)
        {
            throw new VibeFsFormatException(
                $"Unsupported block size {superblock.BlockSize}; expected {FsBlockSize}.");
        }

        if (superblock.InodeCount == 0 || superblock.RootInode == 0 || superblock.RootInode >= superblock.InodeCount)
        {
            throw new VibeFsFormatException("Invalid inode table metadata in superblock.");
        }

        if (superblock.DataBlocks == 0 || superblock.DataStartBlock >= superblock.TotalBlocks)
        {
            throw new VibeFsFormatException("Invalid data-area metadata in superblock.");
        }

        return superblock;
    }

    private byte[] ReadBitmap(uint firstBlock, uint blockCount)
    {
        var bitmap = new byte[blockCount * FsBlockSize];
        for (uint i = 0; i < blockCount; i++)
        {
            var block = new byte[FsBlockSize];
            ReadBlock(firstBlock + i, block);
            Buffer.BlockCopy(block, 0, bitmap, checked((int)(i * FsBlockSize)), (int)FsBlockSize);
        }

        return bitmap;
    }

    private (uint InodeNumber, Inode Inode) ResolvePath(string path)
    {
        var resolved = TryResolvePath(path);
        if (resolved is null)
        {
            throw new FileNotFoundException($"Path '{path}' was not found.", path);
        }

        return resolved.Value;
    }

    private (uint InodeNumber, Inode Inode)? TryResolvePath(string path)
    {
        if (path == "/")
        {
            var root = ReadInode(_superblock.RootInode);
            return (_superblock.RootInode, root);
        }

        var currentInodeNumber = _superblock.RootInode;
        var currentInode = ReadInode(currentInodeNumber);

        var components = path.Split('/', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        foreach (var component in components)
        {
            if (currentInode.Type != (ushort)VibeFsNodeType.Directory)
            {
                return null;
            }

            var match = EnumerateDirectoryEntries(currentInode)
                .FirstOrDefault(entry => string.Equals(entry.Name, component, StringComparison.Ordinal));

            if (match is null)
            {
                return null;
            }

            currentInodeNumber = match.Inode;
            currentInode = ReadInode(currentInodeNumber);
        }

        return (currentInodeNumber, currentInode);
    }

    private IEnumerable<DirectoryEntry> EnumerateDirectoryEntries(Inode directoryInode)
    {
        var blockCount = (uint)(directoryInode.Size / FsBlockSize);
        var block = new byte[FsBlockSize];

        for (uint fileBlock = 0; fileBlock < blockCount; fileBlock++)
        {
            var diskBlock = MapFileBlock(directoryInode, fileBlock);
            if (diskBlock == 0)
            {
                continue;
            }

            ReadBlock(diskBlock, block);
            var offset = 0;
            while (offset < block.Length)
            {
                var span = block.AsSpan(offset);
                if (span.Length < 8)
                {
                    break;
                }

                var inode = BinaryPrimitives.ReadUInt32LittleEndian(span.Slice(0, 4));
                var recordLength = BinaryPrimitives.ReadUInt16LittleEndian(span.Slice(4, 2));
                var nameLength = span[6];
                var type = span[7];

                if (recordLength < 8 || offset + recordLength > block.Length)
                {
                    break;
                }

                if (8 + nameLength <= recordLength)
                {
                    var name = inode == 0
                        ? string.Empty
                        : Encoding.UTF8.GetString(span.Slice(8, nameLength));
                    yield return new DirectoryEntry(
                        inode,
                        recordLength,
                        nameLength,
                        type,
                        name,
                        diskBlock,
                        (ushort)offset);
                }

                offset += recordLength;
            }
        }
    }

    private Inode ReadInode(uint inodeNumber)
    {
        if (inodeNumber >= _superblock.InodeCount)
        {
            throw new VibeFsFormatException($"Inode index {inodeNumber} is out of range.");
        }

        var inodeTableBlock = _superblock.InodeTableBlock + (inodeNumber / InodesPerBlock);
        var inodeIndexInBlock = (int)(inodeNumber % InodesPerBlock);

        var block = new byte[FsBlockSize];
        ReadBlock(inodeTableBlock, block);

        var start = inodeIndexInBlock * InodeSize;
        var span = block.AsSpan(start, InodeSize);

        var direct = new uint[Ndirect];
        for (var i = 0; i < Ndirect; i++)
        {
            direct[i] = BinaryPrimitives.ReadUInt32LittleEndian(span.Slice(20 + (i * 4), 4));
        }

        return new Inode
        {
            Type = BinaryPrimitives.ReadUInt16LittleEndian(span.Slice(0, 2)),
            Links = BinaryPrimitives.ReadUInt16LittleEndian(span.Slice(2, 2)),
            Size = BinaryPrimitives.ReadUInt64LittleEndian(span.Slice(4, 8)),
            Ctime = BinaryPrimitives.ReadUInt32LittleEndian(span.Slice(12, 4)),
            Mtime = BinaryPrimitives.ReadUInt32LittleEndian(span.Slice(16, 4)),
            Direct = direct,
            Indirect = BinaryPrimitives.ReadUInt32LittleEndian(span.Slice(72, 4)),
            Indirect2 = BinaryPrimitives.ReadUInt32LittleEndian(span.Slice(76, 4)),
            Indirect3 = BinaryPrimitives.ReadUInt32LittleEndian(span.Slice(80, 4))
        };
    }

    private void WriteInode(uint inodeNumber, Inode inode)
    {
        var inodeTableBlock = _superblock.InodeTableBlock + (inodeNumber / InodesPerBlock);
        var inodeIndexInBlock = (int)(inodeNumber % InodesPerBlock);

        var block = new byte[FsBlockSize];
        ReadBlock(inodeTableBlock, block);

        var start = inodeIndexInBlock * InodeSize;
        var span = block.AsSpan(start, InodeSize);

        BinaryPrimitives.WriteUInt16LittleEndian(span.Slice(0, 2), inode.Type);
        BinaryPrimitives.WriteUInt16LittleEndian(span.Slice(2, 2), inode.Links);
        BinaryPrimitives.WriteUInt64LittleEndian(span.Slice(4, 8), inode.Size);
        BinaryPrimitives.WriteUInt32LittleEndian(span.Slice(12, 4), inode.Ctime);
        BinaryPrimitives.WriteUInt32LittleEndian(span.Slice(16, 4), inode.Mtime);

        for (var i = 0; i < Ndirect; i++)
        {
            BinaryPrimitives.WriteUInt32LittleEndian(span.Slice(20 + (i * 4), 4), inode.Direct[i]);
        }

        BinaryPrimitives.WriteUInt32LittleEndian(span.Slice(72, 4), inode.Indirect);
        BinaryPrimitives.WriteUInt32LittleEndian(span.Slice(76, 4), inode.Indirect2);
        BinaryPrimitives.WriteUInt32LittleEndian(span.Slice(80, 4), inode.Indirect3);

        WriteBlock(inodeTableBlock, block);
    }

    private uint MapFileBlock(Inode inode, uint fileBlockIndex, bool allocate = false)
    {
        if (fileBlockIndex < Ndirect)
        {
            if (inode.Direct[fileBlockIndex] == 0 && allocate)
            {
                inode.Direct[fileBlockIndex] = AllocateDataBlock();
            }

            return inode.Direct[fileBlockIndex];
        }

        fileBlockIndex -= Ndirect;
        if (fileBlockIndex < PointersPerBlock)
        {
            return ReadPointerFromTree(inode, 1, fileBlockIndex, allocate);
        }

        fileBlockIndex -= PointersPerBlock;
        var doubleLimit = (uint)PointersPerBlock * (uint)PointersPerBlock;
        if (fileBlockIndex < doubleLimit)
        {
            return ReadPointerFromTree(inode, 2, fileBlockIndex, allocate);
        }

        fileBlockIndex -= doubleLimit;
        var tripleLimit = (ulong)PointersPerBlock * (ulong)PointersPerBlock * (ulong)PointersPerBlock;
        if (fileBlockIndex < tripleLimit)
        {
            return ReadPointerFromTree(inode, 3, fileBlockIndex, allocate);
        }

        return 0;
    }

    private uint ReadPointerFromTree(Inode inode, int levels, uint index, bool allocate)
    {
        uint root = levels switch
        {
            1 => inode.Indirect,
            2 => inode.Indirect2,
            3 => inode.Indirect3,
            _ => throw new ArgumentOutOfRangeException(nameof(levels))
        };

        if (root == 0)
        {
            if (!allocate)
            {
                return 0;
            }

            root = AllocateDataBlock();
            switch (levels)
            {
                case 1:
                    inode.Indirect = root;
                    break;
                case 2:
                    inode.Indirect2 = root;
                    break;
                case 3:
                    inode.Indirect3 = root;
                    break;
            }
        }

        var currentBlock = root;
        for (var level = levels; level >= 1; level--)
        {
            var pointers = ReadPointerBlock(currentBlock);
            var entriesCovered = Pow((uint)PointersPerBlock, (uint)(level - 1));
            var slot = (int)(index / entriesCovered);
            index %= entriesCovered;

            if (level == 1)
            {
                if (pointers[slot] == 0 && allocate)
                {
                    pointers[slot] = AllocateDataBlock();
                    WritePointerBlock(currentBlock, pointers);
                }

                return pointers[slot];
            }

            if (pointers[slot] == 0)
            {
                if (!allocate)
                {
                    return 0;
                }

                pointers[slot] = AllocateDataBlock();
                WritePointerBlock(currentBlock, pointers);
            }

            currentBlock = pointers[slot];
        }

        return 0;
    }

    private uint ReadPointer(uint pointerBlock, uint index)
    {
        if (pointerBlock == 0 || index >= PointersPerBlock)
        {
            return 0;
        }

        var pointers = ReadPointerBlock(pointerBlock);
        return pointers[index];
    }

    private uint[] ReadPointerBlock(uint blockNumber)
    {
        if (_pointerBlockCache.TryGetValue(blockNumber, out var cached))
        {
            return cached;
        }

        var block = new byte[FsBlockSize];
        ReadBlock(blockNumber, block);

        var pointers = new uint[PointersPerBlock];
        for (var i = 0; i < pointers.Length; i++)
        {
            pointers[i] = BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(i * 4, 4));
        }

        _pointerBlockCache[blockNumber] = pointers;
        return pointers;
    }

    private void WritePointerBlock(uint blockNumber, uint[] pointers)
    {
        var block = new byte[FsBlockSize];
        for (var i = 0; i < pointers.Length; i++)
        {
            BinaryPrimitives.WriteUInt32LittleEndian(block.AsSpan(i * 4, 4), pointers[i]);
        }

        _pointerBlockCache[blockNumber] = pointers;
        WriteBlock(blockNumber, block);
    }

    private void ReadBlock(uint blockNumber, byte[] destination)
    {
        if (destination.Length != FsBlockSize)
        {
            throw new ArgumentException($"Destination must be exactly {FsBlockSize} bytes.", nameof(destination));
        }

        var offset = (long)blockNumber * FsBlockSize;
        if (offset < 0 || offset >= _stream.Length)
        {
            throw new EndOfStreamException($"Block {blockNumber} is outside image length {_stream.Length}.");
        }

        _stream.Seek(offset, SeekOrigin.Begin);

        var remaining = destination.Length;
        var cursor = 0;
        while (remaining > 0)
        {
            var read = _stream.Read(destination, cursor, remaining);
            if (read <= 0)
            {
                throw new EndOfStreamException($"Failed to read block {blockNumber}: stream ended unexpectedly.");
            }

            remaining -= read;
            cursor += read;
        }
    }

    private void WriteBlock(uint blockNumber, byte[] source)
    {
        if (source.Length != FsBlockSize)
        {
            throw new ArgumentException($"Source must be exactly {FsBlockSize} bytes.", nameof(source));
        }

        EnsureWritable();
        WriteBlockStatic(_stream, blockNumber, source);
    }

    private static void WriteBlockStatic(Stream stream, uint blockNumber, byte[] source)
    {
        var offset = (long)blockNumber * FsBlockSize;
        stream.Seek(offset, SeekOrigin.Begin);
        stream.Write(source, 0, source.Length);
    }

    private void EnsureWritable()
    {
        if (!_canWrite)
        {
            throw new InvalidOperationException("Volume is read-only. Open with VibeFsVolume.OpenReadWrite for mutations.");
        }
    }

    private (uint ParentInodeNumber, Inode ParentInode, string Name) ResolveParentPath(string path)
    {
        var normalized = NormalizeAbsolutePath(path);
        var parts = normalized.Split('/', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        if (parts.Length == 0)
        {
            throw new InvalidOperationException("Root path has no parent.");
        }

        var name = parts[^1];
        if (name.Length > FsNameMax)
        {
            throw new InvalidOperationException($"Name '{name}' exceeds max length {FsNameMax}.");
        }

        var parentPath = parts.Length == 1 ? "/" : "/" + string.Join('/', parts.Take(parts.Length - 1));
        var parent = ResolvePath(parentPath);
        if (parent.Inode.Type != (ushort)VibeFsNodeType.Directory)
        {
            throw new InvalidOperationException($"Parent path '{parentPath}' is not a directory.");
        }

        return (parent.InodeNumber, parent.Inode, name);
    }

    private DirectoryEntry? FindDirectoryEntry(Inode directoryInode, string name)
    {
        return EnumerateDirectoryEntries(directoryInode)
            .FirstOrDefault(x => x.Inode != 0 && string.Equals(x.Name, name, StringComparison.Ordinal));
    }

    private void AddDirectoryEntry(uint directoryInodeNumber, Inode directoryInode, string name, uint targetInode, byte type)
    {
        var nameBytes = Encoding.UTF8.GetBytes(name);
        var requiredLength = Align4(DirentHeaderSize + nameBytes.Length);
        var block = new byte[FsBlockSize];

        var blockCount = (uint)(directoryInode.Size / FsBlockSize);
        for (uint fileBlock = 0; fileBlock < blockCount; fileBlock++)
        {
            var diskBlock = MapFileBlock(directoryInode, fileBlock, allocate: false);
            if (diskBlock == 0)
            {
                continue;
            }

            ReadBlock(diskBlock, block);
            var offset = 0;
            while (offset < block.Length)
            {
                var span = block.AsSpan(offset);
                var inode = BinaryPrimitives.ReadUInt32LittleEndian(span.Slice(0, 4));
                var recordLength = BinaryPrimitives.ReadUInt16LittleEndian(span.Slice(4, 2));
                var nameLength = span[6];
                if (recordLength < DirentHeaderSize || offset + recordLength > block.Length)
                {
                    break;
                }

                if (inode == 0 && recordLength >= requiredLength)
                {
                    WriteDirectoryEntryAt(block, offset, targetInode, nameBytes, type, recordLength);
                    WriteBlock(diskBlock, block);
                    WriteInode(directoryInodeNumber, directoryInode);
                    return;
                }

                if (inode != 0)
                {
                    var used = Align4(DirentHeaderSize + nameLength);
                    if (recordLength - used >= requiredLength)
                    {
                        BinaryPrimitives.WriteUInt16LittleEndian(span.Slice(4, 2), checked((ushort)used));
                        WriteDirectoryEntryAt(block, offset + used, targetInode, nameBytes, type, checked((ushort)(recordLength - used)));
                        WriteBlock(diskBlock, block);
                        WriteInode(directoryInodeNumber, directoryInode);
                        return;
                    }
                }

                offset += recordLength;
            }
        }

        var newDiskBlock = MapFileBlock(directoryInode, blockCount, allocate: true);
        Array.Clear(block);
        WriteDirectoryEntryAt(block, 0, targetInode, nameBytes, type, checked((ushort)FsBlockSize));
        WriteBlock(newDiskBlock, block);
        directoryInode.Size += FsBlockSize;
        WriteInode(directoryInodeNumber, directoryInode);
    }

    private void RemoveDirectoryEntry(uint directoryInodeNumber, Inode directoryInode, string name)
    {
        var block = new byte[FsBlockSize];
        var blockCount = (uint)(directoryInode.Size / FsBlockSize);

        for (uint fileBlock = 0; fileBlock < blockCount; fileBlock++)
        {
            var diskBlock = MapFileBlock(directoryInode, fileBlock, allocate: false);
            if (diskBlock == 0)
            {
                continue;
            }

            ReadBlock(diskBlock, block);
            var offset = 0;
            int previousOffset = -1;

            while (offset < block.Length)
            {
                var span = block.AsSpan(offset);
                var inode = BinaryPrimitives.ReadUInt32LittleEndian(span.Slice(0, 4));
                var recordLength = BinaryPrimitives.ReadUInt16LittleEndian(span.Slice(4, 2));
                var nameLength = span[6];

                if (recordLength < DirentHeaderSize || offset + recordLength > block.Length)
                {
                    break;
                }

                if (inode != 0 && nameLength > 0)
                {
                    var entryName = Encoding.UTF8.GetString(span.Slice(8, nameLength));
                    if (string.Equals(entryName, name, StringComparison.Ordinal))
                    {
                        if (previousOffset >= 0)
                        {
                            var prevLength = BinaryPrimitives.ReadUInt16LittleEndian(block.AsSpan(previousOffset + 4, 2));
                            BinaryPrimitives.WriteUInt16LittleEndian(
                                block.AsSpan(previousOffset + 4, 2),
                                checked((ushort)(prevLength + recordLength)));
                        }
                        else
                        {
                            BinaryPrimitives.WriteUInt32LittleEndian(span.Slice(0, 4), 0);
                        }

                        WriteBlock(diskBlock, block);
                        WriteInode(directoryInodeNumber, directoryInode);
                        return;
                    }
                }

                previousOffset = offset;
                offset += recordLength;
            }
        }

        throw new FileNotFoundException($"Directory entry '{name}' was not found.", name);
    }

    private bool IsDirectoryEmpty(Inode directoryInode)
    {
        foreach (var entry in EnumerateDirectoryEntries(directoryInode))
        {
            if (entry.Inode != 0 && entry.Name is not "." and not "..")
            {
                return false;
            }
        }

        return true;
    }

    private uint AllocateInode()
    {
        for (uint ino = 1; ino < _superblock.InodeCount; ino++)
        {
            if (!GetBit(_inodeBitmap, (int)ino))
            {
                SetBit(_inodeBitmap, (int)ino);
                return ino;
            }
        }

        throw new InvalidOperationException("No free inodes remain.");
    }

    private uint AllocateDataBlock()
    {
        for (uint i = 0; i < _superblock.DataBlocks; i++)
        {
            if (!GetBit(_dataBitmap, (int)i))
            {
                SetBit(_dataBitmap, (int)i);
                var absolute = _superblock.DataStartBlock + i;
                WriteBlock(absolute, new byte[FsBlockSize]);
                return absolute;
            }
        }

        throw new InvalidOperationException("No free data blocks remain.");
    }

    private void FreeDataBlock(uint absoluteBlock)
    {
        if (absoluteBlock < _superblock.DataStartBlock || absoluteBlock >= _superblock.TotalBlocks)
        {
            return;
        }

        var index = absoluteBlock - _superblock.DataStartBlock;
        ClearBit(_dataBitmap, (int)index);
        _pointerBlockCache.Remove(absoluteBlock);
    }

    private void FreeInodeBlocks(Inode inode)
    {
        for (var i = 0; i < Ndirect; i++)
        {
            if (inode.Direct[i] != 0)
            {
                FreeDataBlock(inode.Direct[i]);
                inode.Direct[i] = 0;
            }
        }

        if (inode.Indirect != 0)
        {
            FreeIndirectTree(inode.Indirect, 1);
            inode.Indirect = 0;
        }

        if (inode.Indirect2 != 0)
        {
            FreeIndirectTree(inode.Indirect2, 2);
            inode.Indirect2 = 0;
        }

        if (inode.Indirect3 != 0)
        {
            FreeIndirectTree(inode.Indirect3, 3);
            inode.Indirect3 = 0;
        }

        inode.Size = 0;
    }

    private void FreeIndirectTree(uint blockNumber, int level)
    {
        var pointers = ReadPointerBlock(blockNumber);
        if (level == 1)
        {
            foreach (var pointer in pointers)
            {
                if (pointer != 0)
                {
                    FreeDataBlock(pointer);
                }
            }
        }
        else
        {
            foreach (var pointer in pointers)
            {
                if (pointer != 0)
                {
                    FreeIndirectTree(pointer, level - 1);
                }
            }
        }

        FreeDataBlock(blockNumber);
    }

    private void FlushBitmaps()
    {
        for (uint i = 0; i < _superblock.InodeBitmapBlocks; i++)
        {
            var start = checked((int)(i * FsBlockSize));
            WriteBlock(_superblock.InodeBitmapBlock + i, _inodeBitmap.AsSpan(start, (int)FsBlockSize).ToArray());
        }

        for (uint i = 0; i < _superblock.DataBitmapBlocks; i++)
        {
            var start = checked((int)(i * FsBlockSize));
            WriteBlock(_superblock.DataBitmapBlock + i, _dataBitmap.AsSpan(start, (int)FsBlockSize).ToArray());
        }

        _stream.Flush();
    }

    private static int Align4(int value)
    {
        return (value + 3) & ~3;
    }

    private static uint Pow(uint value, uint exp)
    {
        var result = 1u;
        for (var i = 0u; i < exp; i++)
        {
            result *= value;
        }

        return result;
    }

    private static bool GetBit(byte[] bitmap, int bit)
    {
        return (bitmap[bit >> 3] & (1 << (bit & 7))) != 0;
    }

    private static void SetBit(byte[] bitmap, int bit)
    {
        bitmap[bit >> 3] |= (byte)(1 << (bit & 7));
    }

    private static void ClearBit(byte[] bitmap, int bit)
    {
        bitmap[bit >> 3] &= (byte)~(1 << (bit & 7));
    }

    private static void WriteDirectoryEntryAt(byte[] block, int offset, uint inode, byte[] nameBytes, byte type, ushort recordLength)
    {
        BinaryPrimitives.WriteUInt32LittleEndian(block.AsSpan(offset, 4), inode);
        BinaryPrimitives.WriteUInt16LittleEndian(block.AsSpan(offset + 4, 2), recordLength);
        block[offset + 6] = checked((byte)nameBytes.Length);
        block[offset + 7] = type;
        if (nameBytes.Length > 0)
        {
            Buffer.BlockCopy(nameBytes, 0, block, offset + 8, nameBytes.Length);
        }
    }

    private static void WriteDirectoryEntry(byte[] block, ref int offset, uint inode, string name, byte type, ushort fixedLength)
    {
        var nameBytes = Encoding.UTF8.GetBytes(name);
        WriteDirectoryEntryAt(block, offset, inode, nameBytes, type, fixedLength);
        offset += fixedLength;
    }

    private static string NormalizeAbsolutePath(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
        {
            return "/";
        }

        if (!path.StartsWith('/'))
        {
            path = "/" + path;
        }

        var parts = path.Split('/', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        return parts.Length == 0 ? "/" : "/" + string.Join('/', parts);
    }

    private static string JoinPath(string directoryPath, string name)
    {
        return directoryPath == "/" ? "/" + name : directoryPath + "/" + name;
    }

    private static VibeFsNodeType MapType(ushort inodeType)
    {
        return inodeType switch
        {
            1 => VibeFsNodeType.File,
            2 => VibeFsNodeType.Directory,
            _ => VibeFsNodeType.Unknown
        };
    }

    private sealed record Superblock(
        uint Magic,
        uint Version,
        uint BlockSize,
        uint TotalBlocks,
        uint InodeCount,
        uint InodeBitmapBlock,
        uint InodeBitmapBlocks,
        uint DataBitmapBlock,
        uint DataBitmapBlocks,
        uint InodeTableBlock,
        uint InodeTableBlocks,
        uint DataStartBlock,
        uint DataBlocks,
        uint RootInode);

    private sealed class Inode
    {
        public ushort Type { get; set; }
        public ushort Links { get; set; }
        public ulong Size { get; set; }
        public uint Ctime { get; set; }
        public uint Mtime { get; set; }
        public uint[] Direct { get; set; } = new uint[Ndirect];
        public uint Indirect { get; set; }
        public uint Indirect2 { get; set; }
        public uint Indirect3 { get; set; }

        public static Inode Create(ushort type, ushort links)
        {
            return new Inode
            {
                Type = type,
                Links = links,
                Size = 0,
                Ctime = 0,
                Mtime = 0,
                Direct = new uint[Ndirect],
                Indirect = 0,
                Indirect2 = 0,
                Indirect3 = 0
            };
        }
    }

    private sealed record DirectoryEntry(
        uint Inode,
        ushort RecordLength,
        byte NameLength,
        byte Type,
        string Name,
        uint DiskBlock,
        ushort OffsetInBlock);
}
