# glm-engine → Qviart Dual 4K / OpenPLi 9.2

This repository contains the supplied `glm-engine.cpp` and its bundled GLM/stb headers, wrapped in a small OpenEmbedded layer so the OpenPLi `release-9.2` build environment can compile it into an IPK.

Target confirmed from the receiver:

- Machine: `dual`
- Box: Qviart Dual 4K
- OpenPLi: `9.2-release`
- CPU ABI reported by `uname -m`: `armv7l`
- glibc: `2.33`
- opkg machine architecture: `dual`

## GitHub build

1. Create a new GitHub repository.
2. Upload the contents of this repository (not the outer ZIP folder).
3. Push to `main`, or open **Actions → Build glm-engine IPK for Qviart Dual 4K → Run workflow**.
4. The workflow clones the official OpenPLi `release-9.2` build environment, adds `meta-glm`, runs BitBake for `MACHINE=dual`, validates `libglm.so`, and uploads the resulting `.ipk` as an Actions artifact.

The workflow uses the official OpenPLi `release-9.2` branch rather than a generic ARM cross compiler because `glm-engine.cpp` links against the target's EGL/GLES2, curl, SQLite, FreeType and HarfBuzz stack.

## Result

The package installs:

`/usr/lib/libglm.so`

The recipe deliberately keeps `libglm.so` as an unversioned runtime library because the supplied source is consumed as a shared object by the Enigma2 integration.

## Install on the receiver

After downloading the IPK from GitHub Actions and copying it to the receiver:

```sh
opkg install /tmp/glm-engine_*.ipk
```

Then verify:

```sh
file /usr/lib/libglm.so
readelf -h /usr/lib/libglm.so
readelf -d /usr/lib/libglm.so | grep NEEDED
```

The workflow intentionally does not bundle libc, libstdc++, EGL/GLES2, curl, SQLite, FreeType or HarfBuzz into the IPK; those are expected to come from the OpenPLi target image.
