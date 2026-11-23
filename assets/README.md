# Assets

This repository does not include Amp's Puck art, 3D models, textures, or the
PolySans / Berkeley Mono fonts. Those are copyrighted assets.

## Runtime assets

The native client fetches the public Puck assets from
`https://static.ampcode.com/content/puck/` and caches them under:

```text
$XDG_CACHE_HOME/puck-kiosk/assets/
```

For development, `tools/fetch-assets` downloads the classic fixture into
`assets/cache/` (gitignored) and verifies every file against
`assets/manifest.json`:

```bash
tools/fetch-assets
```

The repository stores only the manifest (URL, size, SHA-256), never the binaries.

The cache key is the content hash in the URL (`puck-<hash>.json`,
`albedo-<hash>.webp`, `normal-<hash>.webp`, `image-<hash>.webp`,
`arms-<hash>.bin`, `props-<hash>.bin`). Treat the cache as disposable.

## Local 2D sprite

For development and for the 2D-only build, place a sprite at:

```text
assets/classic.png
```

The production 2D renderer uses the 498×602 image and the face data
(`eyes`, `mouth`, `body`, `limbs`) from the matching `puck-<hash>.json`. A
placeholder is drawn when no sprite is present.

## Fonts

Do not commit the PolySans or Berkeley Mono files. Pass a font you have the
right to use with `--font /path/to/font.ttf`. The M0 build falls back to the
system DejaVu Sans for development.

## Where should the downloaded resources live?

- **Product repository:** only `assets/manifest.json`, `tools/fetch-assets`, and
the docs. No downloaded binaries, no scraped web bundle, no commercial fonts.
- **Development cache:** `assets/cache/` (gitignored) for local runs. Delete it
at any time; the fetch script recreates it.
- **Private asset mirror:** use a separate private repository or object store
only if you need a reproducible offline fixture set. Put it behind Git LFS or
S3/MinIO, and mirror only `classic` plus the variants you actually test. The full
148-variant set is not worth committing.
- **Scraped site bundle:** keep the JS/CSS/HTML reference outside the product
repository, for example in an internal research bucket or a `research/` folder
in the private mirror. It is Amp's code and changes with every deploy; it is not
a stable build input.
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
