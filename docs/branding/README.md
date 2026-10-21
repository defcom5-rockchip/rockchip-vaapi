# Number Nine logo

The mark is a 9 whose tail is a red chili pepper. The 9 is for Number Nine, and the pepper for Number Nine's "Pepper"
co-processor graphics boards (from 1986), which the releases are named after. The bowl is woven in the Plaid tartan.

## Files
| File | Use |
|---|---|
| `number-nine-lockup.svg` / `-dark.svg` | README and release-page header (mark + wordmark + tagline), light / dark backgrounds |
| `number-nine-lockup-notagline.svg` / `-dark.svg` | header where the tagline doesn't fit |
| `number-nine-mark.svg` | the mark alone, large (≥ 128 px): plaid bowl |
| `number-nine-mark-navy.svg` / `-peach.svg` | the mark at small sizes (≤ 64 px) on light / dark backgrounds |
| `*.png` | pre-rendered: lockups 800 and 2000 px wide; mark 512 / 256 (plaid), 128 / 64 / 32 (navy, peach) |

The mark is taller than wide (about 5:7, the pepper tail hangs below the 9), so the mark PNGs are named by width:
`-512` is 512×717. For a square icon, centre it on a square canvas. GitHub's social preview is not read from the
repository; upload a 1280×640 image under Settings → Social preview.

All text is converted to outlines, so the SVGs render the same everywhere with no fonts installed.

## README header (switches with GitHub's light/dark theme)
```html
<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/branding/number-nine-lockup-dark.svg">
    <img src="docs/branding/number-nine-lockup.svg" alt="Number Nine" width="560">
  </picture>
</p>
```

## Colours
Plaid navy `#1B2A55` · deep navy `#0E1736` · peach `#F3E2B8` · ARMED orange `#E85020` · tagline grey `#586079`
(dark: `#9AA3BD`) · pepper red `#FF5A36 → #E0261B → #9E1310` · stem green `#6CC04A → #2F7D25`.

## Typefaces (outlined in the artwork)
Wordmark: Big Shoulders Display ExtraBold. Tagline: Instrument Sans Regular. Both under the SIL Open Font License 1.1,
which permits their use in logos; no font files are distributed here.

## Licence
The Number Nine logo files in docs/branding/ (SVG and PNG) are © 2026 defcom5-rockchip, all rights reserved. They may
be used to refer to this project — for example in articles, reviews, package listings or link previews. Scaling,
converting between SVG and PNG and placing them on any background are fine; changes to the artwork, colours or
proportions are modifications. Copies of this repository (forks, mirrors, source archives, packages) may include these
files unchanged, but a fork published under another name must not use the logo as its own. Any other use, including
modified versions or use for another project, needs permission: ask in an issue at
https://github.com/defcom5-rockchip/rockchip-vaapi/issues. The logo is not covered by the LGPL-2.1-or-later licence,
which covers the rest of the repository.

The name is a tribute to Number Nine Computer Corporation / Number Nine Visual Technology (1982–2000), whose Pepper
boards the releases are named after. This project is not affiliated with or endorsed by that company or the successors
to its assets, nor by Rockchip; Rockchip is a trademark of its owner.
