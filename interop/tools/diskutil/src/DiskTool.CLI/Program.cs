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
		var diskFile = GetValue(args, "--diskfile");
		if (string.IsNullOrWhiteSpace(diskFile))
		{
			throw new ArgumentException("Missing required argument: --diskfile <path>");
		}

		if (TryGetCommandArgs(args, out var command, out var commandArgs) == false)
		{
			throw new ArgumentException("Specify exactly one command: --import|-i, --export|-e, or --ls.");
		}

		switch (command)
		{
			case "import":
				if (commandArgs.Count != 2)
				{
					throw new ArgumentException("Import expects: --import|-i <local-file> <vibefs-file>");
				}

				ImportFile(diskFile!, commandArgs[0], commandArgs[1]);
				return 0;

			case "export":
				if (commandArgs.Count != 2)
				{
					throw new ArgumentException("Export expects: --export|-e <vibefs-file> <local-file>");
				}

				ExportFile(diskFile!, commandArgs[0], commandArgs[1]);
				return 0;

			case "ls":
				if (commandArgs.Count > 1)
				{
					throw new ArgumentException("List expects: --ls [vibefs-path]");
				}

				var path = commandArgs.Count == 1 ? commandArgs[0] : "/";
				ListPath(diskFile!, path);
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

	var commandFlags = new List<(string Name, int Index)>(3);
	AddIfPresent(args, commandFlags, "import", "--import");
	AddIfPresent(args, commandFlags, "import", "-i");
	AddIfPresent(args, commandFlags, "export", "--export");
	AddIfPresent(args, commandFlags, "export", "-e");
	AddIfPresent(args, commandFlags, "ls", "--ls");

	if (commandFlags.Count == 0)
	{
		return false;
	}

	var distinctCommands = commandFlags.Select(x => x.Name).Distinct(StringComparer.Ordinal).ToArray();
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
	if (File.Exists(localPath) == false)
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
	if (string.IsNullOrWhiteSpace(localDir) == false)
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
		var type = entry.Type == VibeFsNodeType.Directory ? "dir " : "file";
		Console.WriteLine($"{type}  {entry.Size,10}  {entry.FullPath}");
	}
}

static void PrintUsage()
{
	Console.WriteLine("DiskTool.CLI - VibeFS disk utility");
	Console.WriteLine();
	Console.WriteLine("Usage:");
	Console.WriteLine("  disktool-cli --diskfile <diskfile> --import|-i <local-file> <vibefs-file>");
	Console.WriteLine("  disktool-cli --diskfile <diskfile> --export|-e <vibefs-file> <local-file>");
	Console.WriteLine("  disktool-cli --diskfile <diskfile> --ls [vibefs-path]");
}
