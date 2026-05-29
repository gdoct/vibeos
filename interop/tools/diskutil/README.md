# Disk utility

This utility allows access to the virtual disk images created by the operating system. It can be used to read and write data to the virtual disk images, as well as to manage the disk images themselves.
the program works as an interactive shell, allowing users to enter commands to perform various operations on the disk images. The utility supports a range of commands for managing disk images, including creating new disk images, mounting and unmounting disk images, and reading and writing data to disk images.

the supported commands are as follows:
- `ls`: List the contents of the current directory on the disk image.
- `cd [directory]`: Change the current directory on the disk image.
- `cat [file]`: Display the contents of a file on the disk image.
- `write [file] [data]`: Write data to a file on the disk image.
- `mkdir [directory]`: Create a new directory on the disk image.
- `copy [source] [destination]`: Copy a file or directory from one location to another on the disk image.
- `rm [file/directory]`: Remove a file or directory from the disk image.
- `exit`: Exit the disk utility.

## Usage
To use the disk utility, run the following command:

```diskutil [path-to-disk-image] [options]```
```


# Filesystem logic
Copy the filesystem source files from the kernel to the `diskutil` directory into a folder with a version number. This will allow the disk utility to access the necessary filesystem logic for reading and writing data to the disk images. When we later update the filesystem logic we can copy that to a new folder so we can stay backwards compatible with older disk images that may still be using the older filesystem logic.