# Disk utility

Cross-platform C# utility for interacting with virtual disk images used by the OS project.

## Current status
The solution is scaffolded and includes a working VibeFS explorer with write operations.

Projects:
- `DiskUtil.Core` (`src/DiskUtil.Core`): Core library for filesystem and disk image logic.
- `DiskUtil.Web` (`src/DiskUtil.Web`): ASP.NET Core Razor Pages web UI.
- `DiskUtil.Core.Tests` (`tests/DiskUtil.Core.Tests`): xUnit tests for core logic.

Project dependencies:
- `DiskUtil.Web` -> `DiskUtil.Core`
- `DiskUtil.Core.Tests` -> `DiskUtil.Core`

Solution file:
- `DiskUtil.sln`

## Build and run

Build everything:

```bash
dotnet build DiskUtil.sln
```

Run the web UI:

```bash
dotnet run --project src/DiskUtil.Web
```

Run the web UI with a specific default image path:

```bash
dotnet run --project src/DiskUtil.Web -- --image /home/guido/myos/vdisk.img
```

You can also set the default in config or environment:
- `src/DiskUtil.Web/appsettings.json` -> `DiskUtil:DefaultImagePath`
- environment variable -> `DiskUtil__DefaultImagePath`

Run tests:

```bash
dotnet test DiskUtil.sln
```

Build and run the CLI:

```bash
dotnet build src/DiskTool.CLI
./src/DiskTool.CLI/bin/Debug/net8.0/disktool-cli --help
```

CLI examples:

```bash
./src/DiskTool.CLI/bin/Debug/net8.0/disktool-cli --diskfile /home/guido/myos/vdisk.img --import ./local.txt /docs/local.txt
./src/DiskTool.CLI/bin/Debug/net8.0/disktool-cli --diskfile /home/guido/myos/vdisk.img --export /docs/local.txt ./local-out.txt
./src/DiskTool.CLI/bin/Debug/net8.0/disktool-cli --diskfile /home/guido/myos/vdisk.img --ls /docs
```

## Implemented structure
Filesystem logic in `DiskUtil.Core` should be versioned to preserve compatibility as formats evolve.

Suggested namespace/folder layout:

```text
DiskUtil.Core/
    Filesystems/
        VibeFS/
            v1_0/
```

## Implemented features
- VibeFS v1.0 reader in `DiskUtil.Core`:
    - Superblock and inode parsing
    - Directory traversal by absolute path
    - File reads with direct + single/double/triple-indirect block support
    - File and directory mutations: format image, create directory, write/overwrite file, copy file, delete file/empty directory
- Explorer UI in `DiskUtil.Web`:
    - Open disk image by path
    - Browse directory contents
    - Preview file text/hex (first 2 KiB)
    - Download files (up to 64 MiB in this build)
    - Create directory
    - Upload file
    - Copy file
    - Delete file or empty directory
- Tests in `DiskUtil.Core.Tests`:
    - In-memory format + read tests
    - Round-trip write and reopen verification
    - Create/copy/delete behavior tests
    - Invalid magic rejection test

## Current limitations
- Web delete only supports files and empty directories.
- The web app assumes the image starts with a VibeFS superblock at block 0.