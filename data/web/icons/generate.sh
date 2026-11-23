#!/bin/sh
# generate.sh - make the bitmap icons from the SVG sources in this directory
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Needs rsvg-convert (librsvg) and magick (ImageMagick 7).  Run it by path
# whenever favicon.svg or app-icon.svg changes, and commit what it writes:
# the build never runs it.  See README.md.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Tab icon: the SVG is served as it is; these are for browsers that do not
# take an SVG icon.  16, 32 and 48 go into the one .ico file.
for size in 16 32 48; do
	rsvg-convert -w "$size" -h "$size" "$here/favicon.svg" -o "$work/favicon-$size.png"
done
cp "$work/favicon-32.png" "$here/favicon-32.png"
magick "$work/favicon-16.png" "$work/favicon-32.png" "$work/favicon-48.png" "$here/favicon.ico"

# Home screen icons come from app-icon.svg, which is opaque.  PNG colour
# type 2 (truecolour, no alpha channel) guarantees no transparent pixel.
# The maskable icon is the same picture: its mark already sits inside the
# central safe area and its background fills the square.
render() {
	rsvg-convert -w "$1" -h "$1" "$here/app-icon.svg" -o "$work/render-$1.png"
	magick "$work/render-$1.png" -alpha off -define png:color-type=2 "$2"
}

render 180 "$here/apple-touch-icon.png"
render 192 "$here/icon-192.png"
render 512 "$here/icon-512.png"
render 512 "$here/icon-maskable-512.png"

echo "wrote the icons in $here"
