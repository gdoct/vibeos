# Local Doom test data

Drop legal test IWADs here when you want to exercise the package without
committing game data to git.

Suggested files:

- `freedoom1.wad` — free Doom-compatible IWAD for Ultimate Doom-style content
- `freedoom2.wad` — free Doom-compatible IWAD for Doom II-style content

`./build.sh` mirrors the package source tree into the guest image under
`/dist/src/doom/`, so these files become available at:

- `/dist/src/doom/local/freedoom1.wad`
- `/dist/src/doom/local/freedoom2.wad`

Typical guest-side test flow:

```sh
pkg extract /dist/packages/doom-1.0.0.pkg /
doom -iwad /dist/src/doom/local/freedoom1.wad
```