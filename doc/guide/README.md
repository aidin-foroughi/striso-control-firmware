# Quick start guide generator

`build.py` makes the quick start guide for this fork's firmware:

| Output | What it is |
|---|---|
| `doc/quick_start_guide.pdf` | A4 landscape, 4 sides: an 8-page A5 booklet imposed on 2 sheets. Print double-sided (flip on the **short** edge), put sheet 2 inside sheet 1, fold. |
| `doc/quick_start_guide_a5.pdf` | The same 8 pages one by one, for reading on screen. |
| `doc/strisoboard-settings.svg` | The settings card on its own. |
| `doc/strisoboard-settings.png` | The card as a picture, for the README. |

Everything is generated from the tables at the top of `build.py`: the board geometry (from
`synth_control.cpp` and `button_read.c`, the same numbers striso-live uses to draw the board), the
settings keys (`settings_key_table` in `src/synth/synth_control.cpp`, `led_view.hpp`), the voices
and their presets (awva's `voice_*.h`, the `awva` submodule) and the factory tunings
(`config_schema.hpp`). Before it builds, the
script checks those tables against the firmware source and stops if they differ, so when a key or a
preset changes, update the table and the guide follows.

## Rebuilding

```sh
python doc/guide/build.py
```

Needs Python 3 (standard library only) and Microsoft Edge, which prints the pages to PDF headless
(`msedge --headless --print-to-pdf`). Any Chromium works: set `EDGE=<path to the executable>` if it
is not found. `--html-only` writes the HTML and the SVG without printing.

Intermediate files go to `doc/guide/build/` (git-ignored): `a5.html`, `a4.html` (the imposition)
and `card.html`. Open `a5.html` in a browser to check a change quickly; it shows the pages as
148 × 210 mm boxes.

If an Edge window is already open, `msedge.exe` hands the print job to that instance and returns
at once; the script then waits for the file to appear (it can take half a minute).

## Files

- `build.py`: the data, the SVG drawings, the page texts and the build.
- `logo.svg`: the Striso wordmark by Piers Titus van der Torren, taken unchanged from the "Logo"
  layer of `../strisoboard-reference.svg`. It is only used to show which instrument the guide is
  about.
- `Inter.ttf`, `OFL-Inter.txt`: the Inter typeface (SIL Open Font License), subset to Latin,
  punctuation, arrows and symbols. Flats and sharps on the cover are drawn as paths, since Inter has
  no glyphs for them.

## The old sources

- `../quick_start_guide.svg` is the source of the official v2.2 guide by Piers Titus van der Torren
  (Inkscape). It is kept for reference and is no longer used to build the PDF.
- `../strisoboard-settings.svg` used to be that guide's hand-drawn settings card (v2.2.2). It is now
  generated; the old drawing is in the git history and inside `quick_start_guide.svg`.
- `../strisoboard-reference.svg/png` and `../striso_notelayout.svg` are unchanged.

## Page plan

1. Cover: the board with note names and the control buttons.
2. Thank you, Piers; about this fork; getting started.
3. The sounds: the nine engines, the voice and preset table.
4. Settings: switch and dial keys, the LED value scale, nudge and circle, choosing voices and presets.
5. The settings card.
6. Presets, tuning, transposing, reset and factory settings.
7. USB audio, Striso Live, updating firmware.
8. Troubleshooting, credits.

The booklet order is pages (8, 1), (2, 7), (6, 3), (4, 5): the card faces its explanation when the
booklet is open.
