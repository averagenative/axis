<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
# Icons

Everything here is compiled into the `axisd` binary (see `data/web/` and
the Makefile) except this file and `generate.sh`. The icon files are
generated once and committed; the build never needs an image tool.

## Sources (edited by hand)

| File | What it is |
|------|------------|
| `logo-mark.svg` | The mark, as the pages draw it inline (the server builds the same shapes from element nodes in `src/web/axis-web-logo.c`, so the theme can colour it). Kept here as the reference drawing. |
| `favicon.svg` | The tab icon: the mark on a dark rounded tile, fixed colours. Served as it is. |
| `app-icon.svg` | Square, opaque, mark inside the central 66 percent. The source of every home screen icon. |
| `safari-pinned-tab.svg` | One colour; Safari recolours it. Served as it is. |
| `manifest.webmanifest` | The web app manifest. Its icon paths are relative to the manifest, so they follow the versioned static path. |

## Generated (do not edit)

| File | Size | Made from | Used by |
|------|------|-----------|---------|
| `favicon.ico` | 16, 32 and 48 in one file | `favicon.svg` | older browsers, and `/favicon.ico` |
| `favicon-32.png` | 32 | `favicon.svg` | browsers without SVG icons |
| `apple-touch-icon.png` | 180, no alpha channel | `app-icon.svg` | iOS home screen, Safari tiles, `/apple-touch-icon.png` |
| `icon-192.png`, `icon-512.png` | 192, 512 | `app-icon.svg` | Android, through the manifest |
| `icon-maskable-512.png` | 512 | `app-icon.svg` | Android adaptive icons |

## Regenerating

Needs `rsvg-convert` (librsvg) and `magick` (ImageMagick 7). From anywhere:

    data/web/icons/generate.sh

Commit the files it writes. The tests check that every icon a page or the
manifest names exists with the pixel size it states, and that the touch
icon is opaque (PNG colour type 2).

## Colours

The files here use the oxblood palette, the brand's (no longer the default theme, which is midnight): `#4a0000`,
`#7d0000` and `#b00000` for the sweep, `#e30000` for the line, on `#170000`.
The tab icon uses slightly brighter steps and a thicker line so the shape
holds at 16 pixels. A tab icon, a home screen icon and the manifest cannot
follow a per-user theme.
