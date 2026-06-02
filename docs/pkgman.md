# package manager `pkg`

 - installs userspace packages
    - a package is a tarball of files, with an optional post-install script
    - a package is defined in package_info.yml, which specifies the package name, version, dependencies, and the files to be installed
```
name: examplepkg
version: 1.0.0
author: anonymous [anon@some.email]
description: An example package for MyOS
dependencies:
  - libc
  - examplelib >= 1.0.0
files:
  - bin/examplepkg
syminks:
  - bin/examplepkg: /bin/examplepkg
    
```

    - the package manager is a userspace program, but it needs some kernel support:
       - a syscall to install a package from a file descriptor (ROADMAP §4)
       - a way to mark files as "installed by a package" so they can be removed later
    - the package manager will be used to install a basic set of userspace tools
    - the package manager will also be used to install a basic set of userspace libraries, including libc and libm
    - there will be a set of prebuilt packages available in the repo, and users can also create their own packages from source
    add this file/folder layout to the repository to keep the packages separate from the other code in the repo:
```
    + packages
        + Makefile
        + package_list.yml
        + examplepkg
            + package_info.yml
            + bin
                + examplepkg
            + dist
                + examplepkg-1.0.0.pkg
            + src
                + examplepkg.c
            + Makefile

        + examplelib
            + package_info.yml
            + lib
                + libexamplelib.a
            + dist
                + examplelib-1.0.0.pkg
            + src
                + examplelib.c
            + Makefile

```

the built packages should be available in the filesystem at /dist/packages. by default a package should be an archive file (.pkg) in a folder, until the user "installs" it so it can be unpacked and we can create the symlinks to the executables in the bin directory.

therefore, we need a package manager program `pkg` that can read the package_list.yml file, which contains a list of available packages and their dependencies, and then install the packages in the correct order. the package manager should also be able to uninstall packages, which involves removing the symlinks that were installed by the package and updating the package_list.yml file accordingly.

examples:
```
$ pkg install examplepkg
Installing examplepkg-1.0.0...
Installing dependencies: libc, examplelib >= 1.0.0...
Installing libc-2.0.0...
Installing examplelib-1.0.0...
Installing examplepkg-1.0.0...
Package examplepkg-1.0.0 installed successfully.    

$ pkg uninstall examplepkg
Uninstalling examplepkg-1.0.0...
Removing symlinks for examplepkg...
Package examplepkg-1.0.0 uninstalled successfully.

$ pkg list
Available packages:
- examplepkg-1.0.0 (installed)
- libc-2.0.0 (installed)
- examplelib-1.0.0 (installed)

$ pkg list example
Available packages:
- examplepkg-1.0.0 (installed)
- examplelib-1.0.0 (installed)

$ pkg update  # (out of scope for v1)
Updating package list...
Package list updated successfully.

$ pkg upgrade
Upgrading packages...
Upgrading libc-2.0.0 to libc-2.1.0...
Upgrading examplelib-1.0.0 to examplelib-1.1.0...
Upgrading examplepkg-1.0.0 to examplepkg-1.1.0...
Packages upgraded successfully.
```