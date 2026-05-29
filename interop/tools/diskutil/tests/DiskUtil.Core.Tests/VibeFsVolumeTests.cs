using System.Text;
using DiskUtil.Core.Filesystems.VibeFs.v1_0;

namespace DiskUtil.Core.Tests;

public class VibeFsVolumeTests
{
    [Fact]
    public void Format_ProducesReadableEmptyRoot()
    {
        using var stream = CreateFormattedImage(64);
        using var volume = new VibeFsVolume(stream);

        var entries = volume.ListDirectory("/");

        Assert.Empty(entries);
    }

    [Fact]
    public void WriteAndReadFile_RoundTripsData()
    {
        using var stream = CreateFormattedImage(64);
        using (var volume = new VibeFsVolume(stream))
        {
            volume.WriteFile("/hello.txt", Encoding.UTF8.GetBytes("Hello from write path\n"));
        }

        stream.Position = 0;
        using var reopened = new VibeFsVolume(stream);

        var bytes = reopened.ReadFileBytes("/hello.txt");
        var text = Encoding.UTF8.GetString(bytes);

        Assert.Equal("Hello from write path\n", text);
    }

    [Fact]
    public void CreateCopyDelete_RoundTripBehaviorIsCorrect()
    {
        using var stream = CreateFormattedImage(128);
        using (var volume = new VibeFsVolume(stream))
        {
            volume.CreateDirectory("/docs");
            volume.WriteFile("/docs/a.txt", Encoding.UTF8.GetBytes("A"));
            volume.CopyFile("/docs/a.txt", "/docs/b.txt");

            var docsEntries = volume.ListDirectory("/docs");
            Assert.Equal(2, docsEntries.Count);

            volume.Delete("/docs/a.txt");
        }

        stream.Position = 0;
        using var reopened = new VibeFsVolume(stream);
        var remaining = reopened.ListDirectory("/docs");
        var b = Assert.Single(remaining);
        Assert.Equal("b.txt", b.Name);
        Assert.Equal("A", Encoding.UTF8.GetString(reopened.ReadFileBytes("/docs/b.txt")));
    }

    [Fact]
    public void Constructor_ThrowsWhenMagicIsInvalid()
    {
        using var stream = CreateFormattedImage(64);
        stream.Position = 0;
        stream.WriteByte(0x00);
        stream.Position = 0;

        Assert.Throws<VibeFsFormatException>(() => new VibeFsVolume(stream));
    }

    private static MemoryStream CreateFormattedImage(uint blocks)
    {
        var stream = new MemoryStream();
        VibeFsVolume.Format(stream, blocks);
        stream.Position = 0;
        return stream;
    }
}