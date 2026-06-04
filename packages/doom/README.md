# doom

Scaffold for porting `ozkl/doomgeneric` into the VibeOS package tree.

This package starts as a buildable placeholder so `pkg create packages/doom`
works immediately and does not break `build.sh`. The actual port can replace the
staged documentation-only payload with a real build once the source tree and any
VibeOS patches are in place.

Recommended next steps:

- place the DoomGeneric source snapshot under `src/`
- add any VibeOS-specific patches under `patches/`
- update `package_info.yml` with a `build:` section that compiles the port under
  `src/`
- change `stage.include` to archive the built game binary and any required data
  files instead of the placeholder documentation