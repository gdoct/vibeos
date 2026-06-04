using DiskUtil.Core.Filesystems.VibeFs.v1_0;

return Run(args);

static int Run(string[] args)
{
    if (args.Length == 0 || HasFlag(args, "--help") || HasFlag(args, "-h"))
    {
        PrintUsage();
        return 0;
    }

    try
    {
        if (!TryGetCommandArgs(args, out var command, out var commandArgs))
        {
            throw new ArgumentException("Specify exactly one command: --import|-i, --export|-e, --ls, --format, --mkdir, --symlink, or --create-volume.");
        }

        if (command == "create-volume")
        {
            if (commandArgs.Count != 2)
            {
                throw new ArgumentException("Create volume expects: --create-volume <size> <outputfile>");
            }

            CreateVolume(commandArgs[0], commandArgs[1]);
            return 0;
        }

        var diskFile = GetValue(args, "--diskfile");
        if (string.IsNullOrWhiteSpace(diskFile))
        {
            throw new ArgumentException("Missing required argument: --diskfile <path>");
        }

        switch (command)
        {
            case "import":
                if (commandArgs.Count != 2)
                {
                    throw new ArgumentException("Import expects: --import|-i <local-file> <vibefs-file>");
                }

                ImportFile(diskFile, commandArgs[0], commandArgs[1]);
                return 0;

            case "export":
                if (commandArgs.Count != 2)
                {
                    throw new ArgumentException("Export expects: --export|-e <vibefs-file> <local-file>");
                }

                ExportFile(diskFile, commandArgs[0], commandArgs[1]);
                return 0;

            case "ls":
                if (commandArgs.Count > 1)
                {
                    throw new ArgumentException("List expects: --ls [vibefs-path]");
                }

                var path = commandArgs.Count == 1 ? commandArgs[0] : "/";
                ListPath(diskFile, path);
                return 0;

            case "format":
                if (commandArgs.Count != 1)
                {
                    throw new ArgumentException("Format expects: --format <total-blocks>");
                }

                if (!uint.TryParse(commandArgs[0], out var totalBlocks))
                {
                    throw new ArgumentException("Format argument <total-blocks> must be an unsigned integer.");
                }

                FormatDisk(diskFile, totalBlocks);
                return 0;

            case "mkdir":
                if (commandArgs.Count != 1)
                {
                    throw new ArgumentException("Mkdir expects: --mkdir|-m <vibefs-path>");
                }

                CreateDirectory(diskFile, commandArgs[0]);
                return 0;

            case "symlink":
                if (commandArgs.Count != 2)
                {
                    throw new ArgumentException("Symlink expects: --symlink <target> <vibefs-linkpath>");
                }

                CreateSymlink(diskFile, commandArgs[0], commandArgs[1]);
                return 0;

            default:
                throw new ArgumentException($"Unsupported command '{command}'.");
        }
    }
    catch (Exception ex)
    {
        Console.Error.WriteLine($"error: {ex.Message}");
        Console.Error.WriteLine();
        PrintUsage();
        return 1;
    }
}

static bool TryGetCommandArgs(string[] args, out string command, out List<string> commandArgs)
{
    command = string.Empty;
    commandArgs = new List<string>();

    var commandFlags = new List<(string Name, int Index)>();
    AddIfPresent(args, commandFlags, "import", "--import");
    AddIfPresent(args, commandFlags, "import", "-i");
    AddIfPresent(args, commandFlags, "export", "--export");
    AddIfPresent(args, commandFlags, "export", "-e");
    AddIfPresent(args, commandFlags, "ls", "--ls");
    AddIfPresent(args, commandFlags, "format", "--format");
    AddIfPresent(args, commandFlags, "mkdir", "--mkdir");
    AddIfPresent(args, commandFlags, "mkdir", "-m");
    AddIfPresent(args, commandFlags, "symlink", "--symlink");
    AddIfPresent(args, commandFlags, "create-volume", "--create-volume");

    if (commandFlags.Count == 0)
    {
        return false;
    }

    var distinctCommands = commandFlags
        .Select(x => x.Name)
        .Distinct(StringComparer.Ordinal)
        .ToArray();

    if (distinctCommands.Length != 1)
    {
        return false;
    }

    command = distinctCommands[0];
    var commandIndex = commandFlags.Min(x => x.Index);

    for (var i = commandIndex + 1; i < args.Length; i++)
    {
        var token = args[i];
        if (token.StartsWith('-'))
        {
            break;
        }

        commandArgs.Add(token);
    }

    return true;
}

static void AddIfPresent(string[] args, List<(string Name, int Index)> collector, string name, string flag)
{
    var index = Array.FindIndex(args, x => string.Equals(x, flag, StringComparison.Ordinal));
    if (index >= 0)
    {
        collector.Add((name, index));
    }
}

static bool HasFlag(string[] args, string flag)
{
    return Array.Exists(args, arg => string.Equals(arg, flag, StringComparison.Ordinal));
}

static string? GetValue(string[] args, string key)
{
    for (var i = 0; i < args.Length - 1; i++)
    {
        if (string.Equals(args[i], key, StringComparison.Ordinal))
        {
            return args[i + 1];
        }
    }

    return null;
}

static void ImportFile(string diskFile, string localPath, string vibeFsPath)
{
    if (!File.Exists(localPath))
    {
        throw new FileNotFoundException($"Local file '{localPath}' was not found.", localPath);
    }

    var bytes = File.ReadAllBytes(localPath);
    using var volume = VibeFsVolume.OpenReadWrite(diskFile);
    volume.WriteFile(vibeFsPath, bytes);
    Console.WriteLine($"Imported '{localPath}' -> '{vibeFsPath}'");
}

static void ExportFile(string diskFile, string vibeFsPath, string localPath)
{
    using var volume = VibeFsVolume.Open(diskFile);
    var bytes = volume.ReadFileBytes(vibeFsPath);

    var localDir = Path.GetDirectoryName(localPath);
    if (!string.IsNullOrWhiteSpace(localDir))
    {
        Directory.CreateDirectory(localDir);
    }

    File.WriteAllBytes(localPath, bytes);
    Console.WriteLine($"Exported '{vibeFsPath}' -> '{localPath}'");
}

static void ListPath(string diskFile, string vibeFsPath)
{
    using var volume = VibeFsVolume.Open(diskFile);
    var normalizedPath = string.IsNullOrWhiteSpace(vibeFsPath) ? "/" : vibeFsPath;
    var entries = volume.ListDirectory(normalizedPath);

    if (entries.Count == 0)
    {
        Console.WriteLine("(empty)");
        return;
    }

    foreach (var entry in entries)
    {
        var type = entry.Type switch
        {
            VibeFsNodeType.Directory => "dir ",
            VibeFsNodeType.Symlink => "link",
            _ => "file"
        };
        Console.WriteLine($"{type}  {entry.Size,10}  {entry.FullPath}");
    }
}

static void FormatDisk(string diskFile, uint totalBlocks)
{
    using var stream = File.Open(diskFile, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.Read);
    VibeFsVolume.Format(stream, totalBlocks);
    Console.WriteLine($"Formatted '{diskFile}' with {totalBlocks} blocks.");
}

static void CreateDirectory(string diskFile, string vibeFsPath)
{
    using var volume = VibeFsVolume.OpenReadWrite(diskFile);
    volume.CreateDirectory(vibeFsPath);
    Console.WriteLine($"Created directory '{vibeFsPath}'.");
}

static void CreateSymlink(string diskFile, string target, string linkPath)
{
    using var volume = VibeFsVolume.OpenReadWrite(diskFile);
    volume.CreateSymlink(linkPath, target);
    Console.WriteLine($"Created symlink '{linkPath}' -> '{target}'.");
}

static void CreateVolume(string sizeArg, string outputFile)
{
    const ulong blockSizeBytes = 4096;
    var totalBytes = ParseSizeToBytes(sizeArg);
    if (totalBytes < blockSizeBytes * 16)
    {
        throw new ArgumentException("Volume size is too small. Minimum is 16 blocks (65536 bytes).", nameof(sizeArg));
    }

    var blocks = checked((uint)((totalBytes + blockSizeBytes - 1) / blockSizeBytes));
    var outputDir = Path.GetDirectoryName(outputFile);
    if (!string.IsNullOrWhiteSpace(outputDir))
    {
        Directory.CreateDirectory(outputDir);
    }

    using var stream = File.Open(outputFile, FileMode.Create, FileAccess.ReadWrite, FileShare.Read);
    VibeFsVolume.Format(stream, blocks);
    Console.WriteLine($"Created formatted volume '{outputFile}' ({blocks} blocks, {blocks * blockSizeBytes} bytes).");
}

static ulong ParseSizeToBytes(string sizeArg)
{
    if (string.IsNullOrWhiteSpace(sizeArg))
    {
        throw new ArgumentException("Size is required.", nameof(sizeArg));
    }

    var trimmed = sizeArg.Trim();
    var unit = trimmed[^1];
    var numberPart = trimmed;
    ulong multiplier = 1;

    if (char.IsLetter(unit))
    {
        numberPart = trimmed[..^1];
        multiplier = char.ToUpperInvariant(unit) switch
        {
            'K' => 1024UL,
            'M' => 1024UL * 1024UL,
            'G' => 1024UL * 1024UL * 1024UL,
            _ => throw new ArgumentException("Size suffix must be one of: K, M, G (example: 64M).", nameof(sizeArg))
        };
    }

    if (!ulong.TryParse(numberPart, out var value) || value == 0)
    {
        throw new ArgumentException("Size value must be a positive integer (example: 64M or 1048576).", nameof(sizeArg));
    }

    return checked(value * multiplier);
}

static void PrintUsage()
{
    Console.WriteLine("DiskTool.CLI - VibeFS disk utility");
    Console.WriteLine();
    Console.WriteLine("Usage:");
    Console.WriteLine("  disktool-cli --create-volume <size> <outputfile>");
    Console.WriteLine("  disktool-cli --diskfile <diskfile> --import|-i <local-file> <vibefs-file>");
    Console.WriteLine("  disktool-cli --diskfile <diskfile> --export|-e <vibefs-file> <local-file>");
    Console.WriteLine("  disktool-cli --diskfile <diskfile> --ls [vibefs-path]");
    Console.WriteLine("  disktool-cli --diskfile <diskfile> --format <total-blocks>");
    Console.WriteLine("  disktool-cli --diskfile <diskfile> --mkdir|-m <vibefs-path>");
    Console.WriteLine("  disktool-cli --diskfile <diskfile> --symlink <target> <vibefs-linkpath>");
    Console.WriteLine();
    Console.WriteLine("Size examples for --create-volume: 65536, 64K, 128M, 1G");
}
