# packages/

Source directories for VibeOS `v1` packages (see [../docs/pkgman.md](../docs/pkgman.md)).

Each subdirectory is a package source containing a `package_info.yml` manifest
and a staged file tree. The image build (`build.sh`) runs `pkg create` on each
of these and copies the resulting `.pkg` archives into `/dist/packages` on the
VibeFS data disk, so they can be listed and extracted on the running system:

    pkg list    /dist/packages/examplepkg-1.0.0.pkg
    pkg extract /dist/packages/examplepkg-1.0.0.pkg /
