# Assets

This repository does not include Amp's Puck art, 3D models, textures, or the
PolySans / Berkeley Mono fonts. Those are copyrighted assets.

## Runtime assets

The client reads `assets/manifest.json` from its working directory, downloads
missing files from `https://static.ampcode.com/content/puck/`, verifies each
file's size and SHA-256, and caches it under:

```text
assets/cache/
```

The cache key is the content hash in the URL (`puck-<hash>.json`,
`albedo-<hash>.webp`, `normal-<hash>.webp`, `image-<hash>.webp`,
`arms-<hash>.bin`, `base-<hash>.bin`, `classic-<hash>.png`). Treat the cache as
disposable. A failed fetch leaves the existing cache in place, and the renderer
falls back to the 2D placeholder.

For development, `tools/fetch-assets` downloads the classic fixture into
`assets/cache/` and verifies every file against `assets/manifest.json`:

```bash
tools/fetch-assets
```

The repository stores only the manifest (URL, size, SHA-256), never the
binaries.

The NSIS installer ships the executable, license, and icon only. It does not
install `assets/manifest.json` or the cache, so an installed copy uses the
placeholder Puck until those files are supplied in its working directory.

## 2D sprite

The 2D renderer prefers:

```text
assets/cache/avatars/classic-<hash>.png
```

and falls back to the opaque WebP sprite
`assets/cache/3d/classic/image-<hash>.webp`. The matching
`assets/cache/3d/classic/puck-<hash>.json` supplies `eyes`, `mouth`, `body`,
`limbs`, `skinColor`, and `scleraColor`. A procedural placeholder is drawn
when neither sprite is present.

## Fonts

Do not commit the PolySans or Berkeley Mono files. Pass a font you have the
right to use with `--font /path/to/font.ttf`. Without it, the client uses a
system font: DejaVu Sans, Liberation Sans, or Noto Sans on Linux, and Segoe UI
or Arial on Windows.

## Where downloaded resources live

- **Product repository:** only `assets/manifest.json`, `tools/fetch-assets`, and
  the documentation. No downloaded binaries, no scraped web bundle, no
  commercial fonts.
- **Development cache:** `assets/cache/` (gitignored) for local runs. Delete it
  at any time; the fetch script recreates it.
- **Private asset mirror:** a separate private repository or object store for a
  reproducible offline fixture set. Put it behind Git LFS or S3/MinIO, and
  mirror only the variants you test.
- **Scraped site bundle:** keep the JS/CSS/HTML reference outside the product
  repository, for example in an internal research bucket or a `research/`
  folder in the private mirror. It is Amp's code and changes with every deploy;
  it is not a stable build input.
- **Fonts:** never commit PolySans or Berkeley Mono. Pass a licensed font with
  `--font`.

A private repository does not change the licensing terms of the assets. Use it
for internal development and interoperability testing, not redistribution.

## Private mirror (optional, access required)

The classic fixture is mirrored in the **private** repository
[4fuu/puck-kiosk-assets-private](https://github.com/4fuu/puck-kiosk-assets-private).
It is not required for a normal build and it is not a git submodule, so
collaborators without access are not blocked.

The mirror matches the `assets/cache/` layout. With access:

```bash
git lfs install
git clone https://github.com/4fuu/puck-kiosk-assets-private.git
cp -r puck-kiosk-assets-private/3d puck-kiosk-assets-private/avatars assets/cache/
```

Otherwise keep using the public fetch path:

```bash
tools/fetch-assets
```
