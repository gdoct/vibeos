using System.Text;
using DiskUtil.Core.Filesystems.MyFs.v1_0;
using Microsoft.AspNetCore.Mvc;
using Microsoft.AspNetCore.Mvc.RazorPages;

namespace DiskUtil.Web.Pages;

public class IndexModel : PageModel
{
    private const ulong PreviewBytesLimit = 2048;
    private const ulong DownloadBytesLimit = 64UL * 1024 * 1024;

    private readonly ILogger<IndexModel> _logger;
    private readonly string? _defaultImagePath;

    public IndexModel(ILogger<IndexModel> logger, IConfiguration configuration)
    {
        _logger = logger;
        _defaultImagePath = configuration["DiskUtil:DefaultImagePath"];
    }

    [BindProperty(SupportsGet = true)]
    public string? ImagePath { get; set; }

    [BindProperty(SupportsGet = true)]
    public string DirectoryPath { get; set; } = "/";

    [BindProperty(SupportsGet = true)]
    public string? PreviewFilePath { get; set; }

    public IReadOnlyList<MyFsNode> Entries { get; private set; } = Array.Empty<MyFsNode>();

    public string? ErrorMessage { get; private set; }

    public string? PreviewText { get; private set; }

    public string? PreviewHex { get; private set; }

    public bool PreviewTruncated { get; private set; }

    [BindProperty]
    public string? NewDirectoryName { get; set; }

    [BindProperty]
    public string? CopySourcePath { get; set; }

    [BindProperty]
    public string? CopyDestinationName { get; set; }

    [TempData]
    public string? StatusMessage { get; set; }

    [TempData]
    public string? ErrorStatusMessage { get; set; }

    public void OnGet()
    {
        ImagePath = ResolveImagePathOrNull(ImagePath);
        if (string.IsNullOrWhiteSpace(ImagePath))
        {
            return;
        }

        try
        {
            using var volume = MyFsVolume.Open(ImagePath);
            DirectoryPath = NormalizePath(DirectoryPath);
            Entries = volume.ListDirectory(DirectoryPath);

            if (!string.IsNullOrWhiteSpace(PreviewFilePath))
            {
                LoadPreview(volume, NormalizePath(PreviewFilePath));
            }
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "Failed to open or read image {ImagePath}", ImagePath);
            ErrorMessage = ex.Message;
            Entries = Array.Empty<MyFsNode>();
        }
    }

    public IActionResult OnGetDownload(string imagePath, string filePath)
    {
        try
        {
            var resolvedImagePath = ResolveImagePathOrThrow(imagePath);
            using var volume = MyFsVolume.Open(resolvedImagePath);
            var node = volume.GetNode(NormalizePath(filePath));
            if (node.Type != MyFsNodeType.File)
            {
                return BadRequest("The selected path is not a file.");
            }

            if (node.Size > DownloadBytesLimit)
            {
                return BadRequest($"Download is limited to {DownloadBytesLimit / (1024 * 1024)} MB in this build.");
            }

            var bytes = volume.ReadFileBytes(node.FullPath, node.Size);
            return File(bytes, "application/octet-stream", node.Name);
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "Failed to download file {FilePath} from image {ImagePath}", filePath, imagePath);
            return BadRequest(ex.Message);
        }
    }

    public IActionResult OnPostCreateDirectory(string imagePath, string directoryPath, string newDirectoryName)
    {
        var resolvedImagePath = ResolveImagePathOrNull(imagePath);
        try
        {
            var parentPath = NormalizePath(directoryPath);
            var name = newDirectoryName.Trim();
            if (string.IsNullOrWhiteSpace(name))
            {
                throw new InvalidOperationException("Directory name is required.");
            }

            using var volume = MyFsVolume.OpenReadWrite(ResolveImagePathOrThrow(imagePath));
            volume.CreateDirectory(JoinPath(parentPath, name));
            StatusMessage = $"Directory '{name}' created.";
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "Failed creating directory {NewDirectoryName} in {DirectoryPath}", newDirectoryName, directoryPath);
            ErrorStatusMessage = ex.Message;
        }

        return RedirectToPage(new { ImagePath = resolvedImagePath, DirectoryPath = NormalizePath(directoryPath) });
    }

    public IActionResult OnPostUpload(string imagePath, string directoryPath, IFormFile uploadFile)
    {
        var resolvedImagePath = ResolveImagePathOrNull(imagePath);
        try
        {
            if (uploadFile is null || uploadFile.Length == 0)
            {
                throw new InvalidOperationException("Choose a file to upload.");
            }

            if ((ulong)uploadFile.Length > DownloadBytesLimit)
            {
                throw new InvalidOperationException($"Uploads are limited to {DownloadBytesLimit / (1024 * 1024)} MB in this build.");
            }

            using var ms = new MemoryStream();
            uploadFile.CopyTo(ms);

            using var volume = MyFsVolume.OpenReadWrite(ResolveImagePathOrThrow(imagePath));
            var targetPath = JoinPath(NormalizePath(directoryPath), Path.GetFileName(uploadFile.FileName));
            volume.WriteFile(targetPath, ms.ToArray());
            StatusMessage = $"Uploaded '{uploadFile.FileName}'.";
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "Failed uploading file to {DirectoryPath}", directoryPath);
            ErrorStatusMessage = ex.Message;
        }

        return RedirectToPage(new { ImagePath = resolvedImagePath, DirectoryPath = NormalizePath(directoryPath) });
    }

    public IActionResult OnPostDelete(string imagePath, string directoryPath, string targetPath)
    {
        var resolvedImagePath = ResolveImagePathOrNull(imagePath);
        try
        {
            using var volume = MyFsVolume.OpenReadWrite(ResolveImagePathOrThrow(imagePath));
            volume.Delete(NormalizePath(targetPath));
            StatusMessage = $"Deleted '{targetPath}'.";
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "Failed deleting {TargetPath}", targetPath);
            ErrorStatusMessage = ex.Message;
        }

        return RedirectToPage(new { ImagePath = resolvedImagePath, DirectoryPath = NormalizePath(directoryPath) });
    }

    public IActionResult OnPostCopy(string imagePath, string directoryPath, string copySourcePath, string copyDestinationName)
    {
        var resolvedImagePath = ResolveImagePathOrNull(imagePath);
        try
        {
            var source = NormalizePath(copySourcePath);
            var destinationName = copyDestinationName.Trim();
            if (string.IsNullOrWhiteSpace(destinationName))
            {
                throw new InvalidOperationException("Destination file name is required.");
            }

            var destination = JoinPath(NormalizePath(directoryPath), destinationName);
            using var volume = MyFsVolume.OpenReadWrite(ResolveImagePathOrThrow(imagePath));
            volume.CopyFile(source, destination);
            StatusMessage = $"Copied '{source}' to '{destination}'.";
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "Failed copy operation from {Source} to {Destination}", copySourcePath, copyDestinationName);
            ErrorStatusMessage = ex.Message;
        }

        return RedirectToPage(new { ImagePath = resolvedImagePath, DirectoryPath = NormalizePath(directoryPath) });
    }

    private string ResolveImagePathOrThrow(string? requestedImagePath)
    {
        var resolved = ResolveImagePathOrNull(requestedImagePath);
        if (string.IsNullOrWhiteSpace(resolved))
        {
            throw new InvalidOperationException(
                "No disk image path was provided. Set one in the form, configure DiskUtil:DefaultImagePath, or pass --image when launching.");
        }

        return resolved;
    }

    private string? ResolveImagePathOrNull(string? requestedImagePath)
    {
        if (!string.IsNullOrWhiteSpace(requestedImagePath))
        {
            return requestedImagePath.Trim();
        }

        if (!string.IsNullOrWhiteSpace(_defaultImagePath))
        {
            return _defaultImagePath.Trim();
        }

        return null;
    }

    private void LoadPreview(MyFsVolume volume, string filePath)
    {
        var node = volume.GetNode(filePath);
        if (node.Type != MyFsNodeType.File)
        {
            ErrorMessage = "Preview is available for files only.";
            return;
        }

        var bytes = volume.ReadFileBytes(filePath, PreviewBytesLimit);
        PreviewTruncated = node.Size > (ulong)bytes.Length;
        PreviewHex = ConvertToHex(bytes);
        PreviewText = ConvertToDisplayText(bytes);
        PreviewFilePath = filePath;
    }

    private static string NormalizePath(string? path)
    {
        if (string.IsNullOrWhiteSpace(path))
        {
            return "/";
        }

        if (!path.StartsWith('/'))
        {
            path = "/" + path;
        }

        var parts = path
            .Split('/', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        return parts.Length == 0 ? "/" : "/" + string.Join('/', parts);
    }

    private static string JoinPath(string directoryPath, string name)
    {
        if (directoryPath == "/")
        {
            return "/" + name;
        }

        return directoryPath + "/" + name;
    }

    private static string ConvertToDisplayText(byte[] bytes)
    {
        if (bytes.Length == 0)
        {
            return string.Empty;
        }

        var printable = bytes.Count(b => b is 9 or 10 or 13 || (b >= 32 && b <= 126));
        if ((double)printable / bytes.Length < 0.85)
        {
            return "[binary content]";
        }

        return Encoding.UTF8.GetString(bytes);
    }

    private static string ConvertToHex(byte[] bytes)
    {
        if (bytes.Length == 0)
        {
            return string.Empty;
        }

        const int bytesPerLine = 16;
        var sb = new StringBuilder();
        for (var i = 0; i < bytes.Length; i += bytesPerLine)
        {
            var lineBytes = bytes.Skip(i).Take(bytesPerLine).ToArray();
            sb.Append(i.ToString("X8"));
            sb.Append("  ");
            sb.Append(string.Join(' ', lineBytes.Select(b => b.ToString("X2"))));
            sb.AppendLine();
        }

        return sb.ToString().TrimEnd();
    }
}
