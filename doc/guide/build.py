#!/usr/bin/env python3
"""
Builds the Striso quick start guide for the expressive synth fork.

Everything is generated from the data at the top of this file (board geometry, the settings key
table, voices and presets): SVG diagrams, HTML pages, and from those the PDFs with headless
Microsoft Edge. See README.md next to this file.

Outputs (paths relative to the repository root):
  doc/quick_start_guide.pdf      A4 landscape, imposed for a folded A5 booklet (print this)
  doc/quick_start_guide_a5.pdf   single A5 pages in reading order (read this on screen)
  doc/strisoboard-settings.svg   the settings card on its own
  doc/strisoboard-settings.png   the same as a picture, for the README
Intermediate HTML lands in doc/guide/build/ (git-ignored).

Usage:  python doc/guide/build.py [--html-only]
"""
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
DOC = os.path.join(REPO, 'doc')
BUILD = os.path.join(HERE, 'build')

# ----------------------------------------------------------------------------------------------
# Board geometry: synth_control.cpp (c0_dis / c1_dis), button_read.c, config_store.c; the same
# tables as striso-live/packages/striso_core/lib/src/layout.dart. Button ids 0..67 are 4 rows of
# 17 columns, row 3 has only 10 keys. c0 counts octaves, c1 fifths from D (button 25 = D4 = 62).
# ----------------------------------------------------------------------------------------------
C0 = [6, 5, 5, 4, 4, 3, 2, 2, 1, 1, 0, 0, -1, -2, -2, -3, -3,
      5, 4, 4, 3, 3, 2, 1, 1, 0, 0, -1, -1, -2, -3, -3, -4, -4,
      4, 3, 3, 2, 2, 1, 0, 0, -1, -1, -2, -2, -3, -4, -4, -5, -5,
      3, 2, 2, 1, 1, 0, -1, -1, -2, -2, -3, -3, -4, -5, -5, -6, -6]
C1 = [c for _ in range(4) for c in range(-8, 9)]
MISSING = {52, 54, 57, 59, 61, 64, 66}
KEY_IDS = [i for i in range(68) if i not in MISSING]
assert len(KEY_IDS) == 61
# config_store.c button_number_map: key number n (0..60, the firmware's F_1 .. B_4 names) -> id
BUTTON_NUMBER_MAP = [56, 51, 63, 58, 53, 65, 60, 55, 67, 62, 40, 35, 47, 42, 37, 49, 44, 39, 34,
                     46, 41, 36, 48, 43, 38, 50, 45, 23, 18, 30, 25, 20, 32, 27, 22, 17, 29, 24,
                     19, 31, 26, 21, 33, 28, 6, 1, 13, 8, 3, 15, 10, 5, 0, 12, 7, 2, 14, 9, 4, 16, 11]
KEY_NUMBER = {bid: n for n, bid in enumerate(BUTTON_NUMBER_MAP)}
MM_COL = 8.5                      # utils/dcompose.py: 8.5 mm per column of fifths
MM_SEMI = 4.5356 * 8.5 / 12       # an octave is 4.5356 columns high
KEY_R = 7.0
LETTERS = ['F', 'C', 'G', 'D', 'A', 'E', 'B']


def pitch(i):
    return 12 * C0[i] + 7 * C1[i]


def spell(c1):
    acc = int(round(c1 / 7.0))
    return LETTERS[c1 - acc * 7 + 3], acc


def key_xy(i):
    """Key centre in mm, y up, origin at D4 (button 25)."""
    return C1[i] * MM_COL, pitch(i) * MM_SEMI


def key_octave(i):
    """Octave number as the firmware and the configuration editor count it (F_1 .. B_4)."""
    return (KEY_NUMBER[i] + 7) // 17 + 1


# Control buttons and leds at the left (mm, same frame as the keys, from strisoboard-reference.svg)
CTRL_X = -89.0
LED_Y = 53.0
GLISS_Y = 31.9
OCT_UP_Y = 1.0
OCT_DOWN_Y = -14.0
SETTINGS_Y = -46.0
UP_LED_Y = (14.5, 10.5)       # UP2 (outer), UP
DOWN_LED_Y = (-27.5, -23.5)   # DOWN2 (outer), DOWN
PLATE = dict(x0=-104.0, x1=79.0, y0=-80.0, y1=80.0, r=9.0)

# ----------------------------------------------------------------------------------------------
# Settings keys: synth_control.cpp config_but() and config_voice() (the current, non OLD layout).
# led: the indicator led colour the key lights, as drawn; knob: what nudging/circling changes.
# Tuning names 1..8 and their colours are the factory settings of config.h (sT<n>name, hT<n>color);
# the configuration editor can change them. Preset colours are config.h hP<n>color.
# ----------------------------------------------------------------------------------------------
G_MIDI, G_SOUND, G_PRESET, G_TUNING, G_SENS, G_VOICE, G_SYSTEM = (
    'MIDI', 'Sound', 'Presets', 'Tuning', 'Sensitivity', 'Voices', 'System')
GROUPS = [
    # name, key tint, key stroke / label colour
    (G_MIDI, '#d9ebf7', '#3b7cab'),
    (G_SOUND, '#fde4cb', '#c0702c'),
    (G_PRESET, '#dcf1da', '#4c8f49'),
    (G_TUNING, '#e7e0f7', '#6f58b5'),
    (G_SENS, '#fcf0c2', '#b8921f'),
    (G_VOICE, '#fbdfe4', '#bf4762'),
    (G_SYSTEM, '#e9e9e9', '#666666'),
]
GROUP_FILL = {g: f for g, f, _ in GROUPS}
GROUP_STROKE = {g: s for g, _, s in GROUPS}


def led_hex(r, g, b):
    """Indicator led colour at full brightness, like striso-live's ledColor()."""
    m = max(r, g, b, 1)
    return '#%02x%02x%02x' % tuple(int(round(v / m * 255)) for v in (r, g, b))


# config_voice(engine, r, g, b): key id -> (engine, led rgb)
VOICE_KEYS = [
    (18, 'strisy', (40, 40, 40)),
    (20, 'va', (64, 20, 0)),
    (22, 'modal', (0, 24, 64)),
    (24, 'bowed', (64, 0, 0)),
    (26, 'wind', (0, 64, 0)),
    (28, 'ep', (40, 0, 64)),
    (13, 'brass', (64, 40, 0)),
]
# voice_*.h NUM_PRESETS / preset_name(); strisy has no presets
VOICES = {
    'strisy': ('strisy', 'the original Striso sound', ['the original sound (no presets)']),
    'va': ('VA', 'virtual analog synth', ['classic', 'pad', 'lead', 'bass', 'brass', 'keys', 'acid', 'strings']),
    'modal': ('modal', 'tuned percussion', ['vibraphone', 'marimba', 'glockenspiel', 'bell', 'kalimba', 'wine glass']),
    'bowed': ('bowed', 'bowed strings', ['violin', 'viola', 'cello', 'bass', 'section', 'ponticello', 'flautando', 'sordino', 'pizzicato']),
    'wind': ('wind', 'woodwinds', ['clarinet', 'bass clarinet', 'soprano sax', 'alto sax', 'tenor sax', 'baritone sax', 'oboe', 'bassoon', 'flute']),
    'ep': ('EP', 'electric pianos', ['stage', 'suitcase', 'mk2', 'dyno', 'ballad', 'bark', 'neosoul', 'fm80s', 'lofi', 'wurli', 'wurlibark', 'pianet', 'felt', 'celesta']),
    'brass': ('brass', 'brass', ['trumpet', 'flugelhorn', 'trombone', 'horn', 'euphonium', 'tuba', 'harmon', 'plunger', 'section']),
}
VOICE_LED = {eng: led_hex(*rgb) for _, eng, rgb in VOICE_KEYS}
VOICE_LED_NAME = {'strisy': 'white', 'va': 'orange', 'modal': 'blue', 'bowed': 'red',
                  'wind': 'green', 'ep': 'purple', 'brass': 'amber'}

TUNINGS = [  # key id, label lines, led colour (config.h hT<n>color at full brightness)
    (34, ['12TET'], '#00ff00'),
    (36, ['31TET'], led_hex(0, 0x55, 0x55)),
    (38, ['19TET'], led_hex(0, 0, 0xaa)),
    (23, ['Pytha-', 'goras'], led_hex(0x55, 0x55, 0)),
    (25, ['5TET'], led_hex(0xaa, 0, 0)),
    (27, ['7TET'], led_hex(0x55, 0, 0x55)),
    (29, ['Bohlen', 'Pierce 1'], led_hex(0x55, 0xaa, 0)),
    (31, ['Bohlen', 'Pierce 2'], led_hex(0x55, 0xaa, 0)),
    (33, ['Just', '7-limit'], led_hex(0, 0xaa, 0x55)),
]
PRESET_COLORS = ['#380000', '#342000', '#383800', '#0e6000', '#003838', '#000ea8', '#300064', '#2a2a2a']
PRESET_KEYS = [35, 37, 39, 41, 43, 45, 30, 32]


def preset_led(i):
    c = PRESET_COLORS[i]
    return led_hex(int(c[1:3], 16), int(c[3:5], 16), int(c[5:7], 16))


class SKey:
    def __init__(self, bid, group, lines, led=None, knob=None, sub=None, note=None, bold=False):
        self.id, self.group, self.lines, self.led, self.knob, self.sub, self.note, self.bold = \
            bid, group, lines, led, knob, sub, note, bold


SETTINGS = [
    # row 1: 0 2 4 (MIDI mode switches; the knob of each sets channels)
    SKey(0, G_MIDI, ['MPE'], led='#00ff00', knob='number of MPE voices (1 to 15)', bold=True),
    SKey(2, G_MIDI, ['Normal'], led=led_hex(8, 1, 0), knob='MIDI channel (1 to 16)'),
    SKey(4, G_MIDI, ['Mono'], led=led_hex(0, 1, 18), knob='MIDI channel (1 to 16)'),
    # row 2: 11 (dis.flip(); with the glissando button held: debug on/off)
    SKey(11, G_SYSTEM, ['flip', '180\u00b0'], led=None),
    # row 3: 17 19 21 6 8
    SKey(17, G_SENS, ['thres-', 'hold'], led='#ff8000', knob='note-on threshold, 0 to 15'),
    SKey(19, G_SENS, ['press', '(z)'], led='#ffff00', knob='pressure, 0 to 3'),
    SKey(21, G_SENS, ['velo-', 'city'], led='#ff00ff', knob='velocity, 0 to 2'),
    SKey(6, G_SENS, ['bend', '(x)'], led='#ff0000', knob='pitch bend, 0 to 4'),
    SKey(8, G_SENS, ['tilt', '(y)'], led='#8000ff', knob='tilt, 0 to 3'),
] + [
    # row 4: 18 20 22 24 26 28 13 ("future use" on the official v2.2 card)
    SKey(bid, G_VOICE, [VOICES[eng][0]], led=led_hex(*rgb), knob='preset', bold=(eng == 'strisy'))
    for bid, eng, rgb in VOICE_KEYS
] + [
    # row 5: 34 36 38 23 25 27 29 31 33
    SKey(bid, G_TUNING, lines, led=led, knob='fine tuning', bold=(bid == 34))
    for bid, lines, led in TUNINGS
] + [
    # row 6: 35 37 39 41 43 45 30 32
    SKey(bid, G_PRESET, [str(i + 1)], led=preset_led(i), bold=(i == 0))
    for i, bid in enumerate(PRESET_KEYS)
] + [
    # row 7: 51 53
    SKey(51, G_SOUND, ['volume'], led='#ffffff', knob='volume, 0 to 127', sub='CC7'),
    SKey(53, G_SOUND, ['decay'], led='#ff00ff', knob='sustain / decay, 0 to 1', sub='CC64'),
    # row 8: 56 58
    SKey(56, G_MIDI, ['MPE', 'bend'], led='#ff0000', knob='12, 24, 48 or 96 semitones'),
    SKey(58, G_MIDI, ['msg', 'rate'], led='#ffff00', knob='message interval, 1 to 15 ms'),
    # row 9: 63 65 67
    SKey(63, G_SYSTEM, ['Panic'], led=led_hex(0x34, 0x20, 0)),
    SKey(65, G_SYSTEM, ['Send', 'cfg'], led=led_hex(0x30, 0, 0x64)),
    SKey(67, G_SYSTEM, ['Reset'], led='#ff0000'),
]
SETTINGS_BY_ID = {k.id: k for k in SETTINGS}
assert len(SETTINGS_BY_ID) == len(SETTINGS)
assert set(SETTINGS_BY_ID) <= set(KEY_IDS)

# Group labels on the card: (group, key id the label sits next to)
CARD_LABELS = [
    (G_MIDI, 0, 'MIDI mode'), (G_SENS, 17, 'Sensitivity'), (G_VOICE, 18, 'Voices'),
    (G_TUNING, 34, 'Tuning'), (G_PRESET, 35, 'Presets'), (G_SOUND, 51, 'Sound'),
    (G_MIDI, 56, 'MIDI'), (G_SYSTEM, 63, 'System'),
]

# ----------------------------------------------------------------------------------------------
# SVG helpers
# ----------------------------------------------------------------------------------------------
INK = '#2a2622'
PLATE_FILL = '#fbeec4'   # the warm Striso board colour, a little lighter for print
PLATE_STROKE = '#8c7a4a'


def esc(s):
    return s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


def f(v):
    return ('%.3f' % v).rstrip('0').rstrip('.')


def accidental_glyph(acc, x, y, size, color=INK):
    """A flat or sharp as a small path (Inter has no U+266D/U+266F), baseline at (x, y)."""
    s = size
    if acc == 0:
        return ''
    if acc < 0:   # flat: stem and a bowl
        d = ('M %s %s L %s %s M %s %s C %s %s %s %s %s %s' % (
            f(x), f(y - 1.0 * s), f(x), f(y + 0.28 * s),
            f(x), f(y - 0.33 * s),
            f(x + 0.55 * s), f(y - 0.62 * s), f(x + 0.62 * s), f(y - 0.05 * s), f(x), f(y + 0.28 * s)))
        return '<path d="%s" fill="none" stroke="%s" stroke-width="%s" stroke-linecap="round"/>' % (
            d, color, f(0.11 * s))
    # sharp: two stems, two thicker slanted bars
    stems = 'M %s %s L %s %s M %s %s L %s %s' % (
        f(x + 0.14 * s), f(y - 0.72 * s), f(x + 0.14 * s), f(y + 0.30 * s),
        f(x + 0.40 * s), f(y - 0.80 * s), f(x + 0.40 * s), f(y + 0.22 * s))
    bars = 'M %s %s L %s %s M %s %s L %s %s' % (
        f(x - 0.02 * s), f(y - 0.10 * s), f(x + 0.56 * s), f(y - 0.30 * s),
        f(x - 0.02 * s), f(y + 0.20 * s), f(x + 0.56 * s), f(y + 0.0 * s))
    return ('<path d="%s" fill="none" stroke="%s" stroke-width="%s" stroke-linecap="round"/>'
            '<path d="%s" fill="none" stroke="%s" stroke-width="%s" stroke-linecap="round"/>' % (
                stems, color, f(0.09 * s), bars, color, f(0.17 * s)))


def plate_rect(x0, x1, y0, y1, r, fill=PLATE_FILL, stroke=PLATE_STROKE, sw=0.6):
    return '<rect x="%s" y="%s" width="%s" height="%s" rx="%s" fill="%s" stroke="%s" stroke-width="%s"/>' % (
        f(x0), f(-y1), f(x1 - x0), f(y1 - y0), f(r), fill, stroke, f(sw))


def triangle(cx, cy, up, size=12.0, fill='#ffffff', stroke=INK, sw=0.5):
    """An octave button: a rounded triangle pointing up or down, with its dark dot."""
    h = size
    w = size * 0.95
    s = 1 if up else -1
    pts = [(cx, cy + s * h / 2), (cx - w / 2, cy - s * h / 2), (cx + w / 2, cy - s * h / 2)]
    d = 'M %s %s L %s %s L %s %s Z' % tuple(f(v) for p in pts for v in (p[0], -p[1]))
    dot_y = cy - s * h * 0.12
    return ('<path d="%s" fill="%s" stroke="%s" stroke-width="%s" stroke-linejoin="round"/>'
            '<circle cx="%s" cy="%s" r="%s" fill="%s"/>' % (d, fill, stroke, f(sw), f(cx), f(-dot_y), f(size * 0.17), INK))


def control_buttons(labels=True, gliss_label='Glissando', settings_label='Settings'):
    """The glissando ring, octave buttons, settings square and the leds, with labels at the left."""
    out = []
    x = CTRL_X
    # indicator led
    out.append('<circle cx="%s" cy="%s" r="1.7" fill="#58c322" stroke="#2f7a0c" stroke-width="0.3"/>' % (f(x), f(-LED_Y)))
    # glissando: a ring with a dot
    out.append('<circle cx="%s" cy="%s" r="6.2" fill="#ffffff" stroke="%s" stroke-width="0.5"/>'
               '<circle cx="%s" cy="%s" r="2.3" fill="%s"/>' % (f(x), f(-GLISS_Y), INK, f(x), f(-GLISS_Y), INK))
    out.append(triangle(x, OCT_UP_Y, True))
    out.append(triangle(x, OCT_DOWN_Y, False))
    for y in UP_LED_Y + DOWN_LED_Y:
        out.append('<circle cx="%s" cy="%s" r="0.9" fill="#e24a4a"/>' % (f(x), f(-y)))
    # settings: a rounded square with a dot
    out.append('<rect x="%s" y="%s" width="12" height="12" rx="2" fill="#ffffff" stroke="%s" stroke-width="0.5"/>'
               '<circle cx="%s" cy="%s" r="2.3" fill="%s"/>' % (f(x - 6), f(-SETTINGS_Y - 6), INK, f(x), f(-SETTINGS_Y), INK))
    if labels:
        lab = [(LED_Y, 'Indicator LED'), (GLISS_Y, gliss_label), (sum(UP_LED_Y) / 2, 'Up LEDs'),
               (OCT_UP_Y, 'Octave up'), (OCT_DOWN_Y, 'Octave down'), (sum(DOWN_LED_Y) / 2, 'Down LEDs'),
               (SETTINGS_Y, settings_label)]
        for y, t in lab:
            out.append('<line x1="%s" y1="%s" x2="%s" y2="%s" stroke="#999" stroke-width="0.3"/>' % (
                f(PLATE['x0'] - 2.5), f(-y), f(PLATE['x0'] - 0.5), f(-y)))
            out.append('<text x="%s" y="%s" font-size="4.3" text-anchor="end" fill="%s">%s</text>' % (
                f(PLATE['x0'] - 4), f(-y + 1.5), INK, esc(t)))
    return '\n'.join(out)


def board_svg(width_mm):
    """The cover diagram: the board with note names, control buttons labelled."""
    left = PLATE['x0'] - 36
    vb = (left, -PLATE['y1'] - 1, PLATE['x1'] - left + 1, PLATE['y1'] - PLATE['y0'] + 2)
    out = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="%s %s %s %s" width="%smm" class="board" '
           'font-family="Inter, Helvetica, Arial, sans-serif">' % (f(vb[0]), f(vb[1]), f(vb[2]), f(vb[3]), f(width_mm))]
    out.append(plate_rect(PLATE['x0'], PLATE['x1'], PLATE['y0'], PLATE['y1'], PLATE['r']))
    # wordmark at the plate's top left corner
    out.append('<image href="logo.svg" x="%s" y="%s" width="26" height="14.7"/>' % (f(PLATE['x0'] + 7), f(-PLATE['y1'] + 5)))
    out.append(control_buttons())
    for i in KEY_IDS:
        x, y = key_xy(i)
        letter, acc = spell(C1[i])
        natural = acc == 0
        out.append('<circle cx="%s" cy="%s" r="%s" fill="%s" stroke="%s" stroke-width="0.45"/>' % (
            f(x), f(-y), f(KEY_R), '#ffffff' if natural else '#cfcbc3', INK))
        lx = x - (1.7 if not natural else 0.9)
        out.append('<text x="%s" y="%s" font-size="5.6" text-anchor="middle" fill="%s">%s</text>' % (
            f(lx), f(-y + 2.0), INK, letter))
        if not natural:
            out.append(accidental_glyph(acc, x + 0.9, -y + 1.7, 3.6))
        out.append('<text x="%s" y="%s" font-size="2.9" text-anchor="middle" fill="%s">%d</text>' % (
            f(x + (3.9 if not natural else 2.6)), f(-y + 4.2), '#555', key_octave(i)))
    out.append('</svg>')
    return '\n'.join(out)


def knob_ring(x, y, r, color):
    return '<circle cx="%s" cy="%s" r="%s" fill="none" stroke="%s" stroke-width="0.55" stroke-dasharray="1.1 1.1"/>' % (
        f(x), f(-y), f(r), color)


def led_dot(x, y, color, r=1.45):
    if color is None:
        return '<circle cx="%s" cy="%s" r="%s" fill="#ffffff" stroke="#8a8a8a" stroke-width="0.3"/>' % (f(x), f(-y), f(r))
    return '<circle cx="%s" cy="%s" r="%s" fill="%s" stroke="#4a4a4a" stroke-width="0.3"/>' % (f(x), f(-y), f(r), color)


def card_svg(width_mm, standalone=False, title='Settings', subtitle=''):
    """The settings card: every key with its function, group colours, led colours, dial rings."""
    x0, x1, y0, y1 = PLATE['x0'], PLATE['x1'], PLATE['y0'], PLATE['y1']
    vb = (x0 - 1, -y1 - 1, x1 - x0 + 2, y1 - y0 + 2)
    head = ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
            'viewBox="%s %s %s %s" width="%smm" height="%smm" class="card" '
            'font-family="Inter, Helvetica, Arial, sans-serif" fill="%s">' % (
                f(vb[0]), f(vb[1]), f(vb[2]), f(vb[3]), f(width_mm), f(width_mm * vb[3] / vb[2]), INK))
    out = [head]
    if standalone:
        out.append('<title>Striso board settings keys, expressive synth fork firmware</title>')
    out.append(plate_rect(x0, x1, y0, y1, PLATE['r'], fill='#fdf6dc'))
    # wordmark top left (where it sits on the real board), title in the gap at the top, subtitle bottom right
    out.append('<image href="logo.svg" x="%s" y="%s" width="22" height="12.5"/>' % (f(x0 + 6), f(-y1 + 3.5)))
    out.append('<text x="-12" y="%s" font-size="7.2" font-weight="700" text-anchor="middle">%s</text>' % (f(-y1 + 7.6), esc(title)))
    for j, line in enumerate(subtitle.split(', ') if subtitle else []):   # top right corner, two lines
        out.append('<text x="%s" y="%s" font-size="2.8" text-anchor="end" fill="#6b6459">%s</text>' % (
            f(x1 - 4), f(-y1 + 4.6 + 3.4 * j), esc(line)))
    # keys
    for i in KEY_IDS:
        x, y = key_xy(i)
        k = SETTINGS_BY_ID.get(i)
        if k is None:
            out.append('<circle cx="%s" cy="%s" r="%s" fill="#f4f1ea" stroke="#c9c3b6" stroke-width="0.35"/>' % (
                f(x), f(-y), f(KEY_R)))
            out.append('<circle cx="%s" cy="%s" r="0.45" fill="#c9c3b6"/>' % (f(x), f(-y)))
            continue
        fill, stroke = GROUP_FILL[k.group], GROUP_STROKE[k.group]
        out.append('<circle cx="%s" cy="%s" r="%s" fill="%s" stroke="%s" stroke-width="0.5"/>' % (
            f(x), f(-y), f(KEY_R), fill, stroke))
        if k.knob:
            out.append(knob_ring(x, y, KEY_R + 1.3, stroke))
        out.append(led_dot(x, y + 4.4, k.led))
        n = len(k.lines)
        size = 3.5 if n == 1 and len(k.lines[0]) <= 6 else 3.0
        if n == 1 and len(k.lines[0]) > 6:
            size = 2.7
        if n == 2:
            size = 2.6
        base = -y + 2.2 if n == 1 else -y + 1.0
        if k.sub:
            base -= 0.8
        for j, line in enumerate(k.lines):
            out.append('<text x="%s" y="%s" font-size="%s" text-anchor="middle"%s>%s</text>' % (
                f(x), f(base + j * (size + 0.5)), f(size), ' font-weight="700"' if k.bold else '', esc(line)))
        if k.sub:
            out.append('<text x="%s" y="%s" font-size="1.9" text-anchor="middle" fill="#6b6459">%s</text>' % (
                f(x), f(base + 2.7), esc(k.sub)))
    # group labels, rotated along the diagonal rows (two columns right = two semitones up)
    ang = -math.degrees(math.atan2(2 * MM_SEMI, 2 * MM_COL))
    for g, bid, text in CARD_LABELS:
        x, y = key_xy(bid)
        ax, ay = x - KEY_R - 3.2, y - 4.6
        if bid == 63:   # System: nothing is left of it, put the label below
            ax, ay = x - 3.5, y - KEY_R - 4.2
        if bid == 56:
            ax, ay = x - KEY_R - 2.6, y - 7.6
        out.append('<text x="%s" y="%s" font-size="4.6" font-weight="600" text-anchor="end" fill="%s" '
                   'transform="rotate(%s %s %s)">%s</text>' % (
                       f(ax), f(-ay), GROUP_STROKE[g], f(ang), f(ax), f(-ay), esc(text)))
    out.append('</svg>')
    return '\n'.join(out)


def led_state(a):
    """led.c led_updown_dial(a): brightness nibble per led (UP2, UP, DOWN, DOWN2), 0..15."""
    ang = ((a + 1024) % 16) - 8
    s = (0xffff >> (2 * ang)) if ang >= 0 else ((0xffff << (2 * -ang)) & 0xffff)
    return [s & 0xf, (s >> 4) & 0xf, (s >> 8) & 0xf, (s >> 12) & 0xf]


def led_scale_svg(width_mm):
    """The up/down leds for the values 0..15, like the picture in the original guide."""
    cols, step = 16, 7.4
    w, h = cols * step + 2, 30
    out = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %s %s" width="%smm" height="%smm" '
           'font-family="Inter, Helvetica, Arial, sans-serif">' % (f(w), f(h), f(width_mm), f(width_mm * h / w))]
    on, half, off = '#e0312f', '#f4b3b2', '#ffffff'
    for a in range(cols):
        cx = 1 + step * (a + 0.5)
        st = led_state(a)
        ys = [3.0, 6.2, 21.8, 25.0]     # UP2, UP, DOWN, DOWN2
        for y, v in zip(ys, st):
            fill = on if v == 0xf else (half if v else off)
            out.append('<circle cx="%s" cy="%s" r="1.15" fill="%s" stroke="%s" stroke-width="0.3"/>' % (
                f(cx), f(y), fill, '#c8302e' if v else '#b0a9a0'))
        # the two octave buttons, small
        for up, cy in ((True, 10.9), (False, 17.1)):
            s = 1 if up else -1
            hh, ww = 5.4, 5.0
            pts = [(cx, cy - s * hh / 2), (cx - ww / 2, cy + s * hh / 2), (cx + ww / 2, cy + s * hh / 2)]
            out.append('<path d="M %s %s L %s %s L %s %s Z" fill="#fff" stroke="%s" stroke-width="0.35" stroke-linejoin="round"/>' % (
                tuple(f(v) for p in pts for v in p) + (INK,)))
        out.append('<text x="%s" y="29" font-size="3.4" text-anchor="middle" fill="%s">%d</text>' % (f(cx), INK, a))
    out.append('</svg>')
    return '\n'.join(out)


def gestures_svg(width_mm):
    """Two keys at real size: one nudged up/down, one circled."""
    out = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 32" width="%smm" height="%smm" '
           'font-family="Inter, Helvetica, Arial, sans-serif" fill="%s">' % (f(width_mm), f(width_mm * 0.32), INK)]
    out.append('<defs><marker id="ah" viewBox="0 0 6 6" refX="5" refY="3" markerWidth="4" markerHeight="4" orient="auto-start-reverse">'
               '<path d="M0,0 L6,3 L0,6 Z" fill="%s"/></marker></defs>' % INK)
    cy = 12.5
    for cx, caption, kind in ((25, ('nudge up or down:', 'one step'), 'nudge'),
                              (75, ('circle: clockwise up,', 'anticlockwise down'), 'circle')):
        out.append('<circle cx="%d" cy="%s" r="7" fill="#fdf6dc" stroke="%s" stroke-width="0.5"/>' % (cx, f(cy), INK))
        out.append('<circle cx="%d" cy="%s" r="1.3" fill="%s"/>' % (cx, f(cy), INK))
        if kind == 'nudge':
            out.append('<line x1="%d" y1="%s" x2="%d" y2="%s" stroke="%s" stroke-width="0.7" marker-end="url(#ah)"/>' % (
                cx, f(cy - 2.5), cx, f(cy - 10.5), INK))
            out.append('<line x1="%d" y1="%s" x2="%d" y2="%s" stroke="%s" stroke-width="0.7" marker-end="url(#ah)"/>' % (
                cx, f(cy + 2.5), cx, f(cy + 10.5), INK))
        else:
            r = 9.6
            out.append('<path d="M %s %s A %s %s 0 1 1 %s %s" fill="none" stroke="%s" stroke-width="0.7" marker-end="url(#ah)"/>' % (
                f(cx - r * 0.7071), f(cy + r * 0.7071), f(r), f(r), f(cx + r * 0.7071), f(cy + r * 0.7071), INK))
        for j, line in enumerate(caption):
            out.append('<text x="%d" y="%s" font-size="2.6" text-anchor="middle">%s</text>' % (cx, f(26.2 + 3.1 * j), esc(line)))
    out.append('</svg>')
    return '\n'.join(out)


def transpose_svg(width_mm):
    """The octave buttons with their plain and settings-held functions."""
    out = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 40" width="%smm" height="%smm" '
           'font-family="Inter, Helvetica, Arial, sans-serif" fill="%s">' % (f(width_mm), f(width_mm * 0.4), INK)]
    cx = 50
    # buttons (y down coordinates here, so draw directly)
    for up, cy in ((True, 13), (False, 27)):
        s = 1 if up else -1
        hh, ww = 11, 10.5
        pts = [(cx, cy - s * hh / 2), (cx - ww / 2, cy + s * hh / 2), (cx + ww / 2, cy + s * hh / 2)]
        out.append('<path d="M %s %s L %s %s L %s %s Z" fill="#fff" stroke="%s" stroke-width="0.5" stroke-linejoin="round"/>' % (
            tuple(f(v) for p in pts for v in p) + (INK,)))
        out.append('<circle cx="%d" cy="%s" r="1.9" fill="%s"/>' % (cx, f(cy + s * 1.3), INK))
    # brace for "both"
    out.append('<path d="M 41 9 Q 36 9 36 14 L 36 18 Q 36 20 34 20 Q 36 20 36 22 L 36 26 Q 36 31 41 31" fill="none" stroke="%s" stroke-width="0.4"/>' % INK)
    out.append('<path d="M 59 9 Q 64 9 64 14 L 64 18 Q 64 20 66 20 Q 64 20 64 22 L 64 26 Q 64 31 59 31" fill="none" stroke="%s" stroke-width="0.4"/>' % INK)
    L = [(10, 'Octave up'), (21, 'Free transpose'), (32, 'Octave down')]
    R = [(10, 'Semitone up'), (21, 'Reset transpose'), (32, 'Semitone down')]
    for y, t in L:
        out.append('<text x="32" y="%d" font-size="3.6" text-anchor="end">%s</text>' % (y + 1.3, t))
    for y, t in R:
        out.append('<text x="68" y="%d" font-size="3.6">%s</text>' % (y + 1.3, t))
    out.append('<text x="68" y="4.5" font-size="3.2" fill="#6b6459">with Settings held:</text>')
    out.append('<text x="32" y="4.5" font-size="3.2" text-anchor="end" fill="#6b6459">alone:</text>')
    out.append('</svg>')
    return '\n'.join(out)


def swatch(color):
    return '<span class="sw" style="background:%s"></span>' % color


# ----------------------------------------------------------------------------------------------
# Pages
# ----------------------------------------------------------------------------------------------
FORK_URL = 'github.com/aidin-foroughi/striso-control-firmware'
LIVE_URL = 'github.com/aidin-foroughi/striso-live'


def git_describe():
    try:
        return subprocess.check_output(['git', 'describe', '--tags', '--always'], cwd=REPO, text=True).strip()
    except Exception:
        return 'unknown'


def page_cover():
    return '''
<div class="cover-head">
  <img class="logo" src="../logo.svg" alt="Striso">
  <div>
    <h1>Quick start guide</h1>
    <div class="kicker">Striso board with the expressive synth fork firmware</div>
  </div>
</div>
<p class="lead">The Striso board is a standalone instrument: plug in headphones or a speaker and play.
With this firmware it carries seven sound engines (bowed strings, woodwinds, brass, electric pianos,
tuned percussion, a virtual analog synth and the original strisy sound), plays over USB as a sound card,
and still works as an MPE MIDI controller.</p>
%s
<p class="caption"><b>61 keys, 4 buttons.</b> Every key senses pressure, strike velocity and tilt in two
directions. The isomorphic layout gives every chord and scale one shape in every key: a fifth is one column
to the right, an octave is straight up. Octave numbers are the ones the configuration editor uses; D<sub>3</sub>
in the middle sounds the D above middle C.</p>
<div class="unofficial">Unofficial fork firmware, %s. Official firmware and support: striso.com, forum.striso.com.</div>
''' % (board_svg(126), FORK_URL)


def page_thanks():
    return '''
<h2>Thank you, Piers</h2>
<p>The Striso is one of the most expressive instruments around. Every key senses pressure, strike velocity and
tilt in two directions, and pressing one feels like touching a string, not pushing a button. The Striso is
designed and built by <b>Piers Titus van der Torren</b>, along with its hardware, firmware, bootloader,
configuration editor and this guide&rsquo;s original text and drawings: all of it thoughtful, careful work.</p>
<p>Piers released the firmware as <b>open source</b> under the GPL. That is the only reason this fork can exist:
it builds directly on his firmware, his key sensing, his MIDI/MPE implementation and his bootloader. Thank you
for making something this good and for sharing it. If you like what you hear, the instrument deserves the
credit: <b>striso.com</b>, <b>forum.striso.com</b>.</p>

<h2>About this fork</h2>
<p>This is an <b>unofficial fork</b>, not supported by Striso. Its additions were vibe coded: written with an AI
coding assistant (Claude Code), directed by a player saying what sounded wrong. They were tested on a real
Striso board, and they work really well. It is still a fork made for fun, so keep a backup of your current
firmware (copy <span class="mono">CURRENT.UF2</span>, see <i>Updating firmware</i>).</p>
<p>Everything the official firmware does still works: MPE and normal MIDI, tunings, presets, the configuration
editor and the UF2 bootloader. <b>Striso Live</b>, the desktop app on page 7, is an optional companion; nothing
needs it.</p>

<h2>Getting started</h2>
<p><b>Standalone:</b> plug the USB cable into your Striso board and a power source, such as a phone charger,
computer or power bank. Plug a headphone or speaker into the headphone / line out mini-jack and start playing.</p>
<p><b>MIDI controller:</b> connect your Striso board to a synthesizer, computer or iPad with the USB cable or a
TRS-MIDI type A adapter. If the synthesizer doesn&rsquo;t support MPE, change the MIDI mode to &lsquo;Normal&rsquo;
(see the settings card).</p>
<p><b>Buttons:</b> the <b>octave up</b> and <b>down</b> buttons shift the sound up to two octaves up or down.
The <b>glissando</b> button makes a smooth transition from one note to another. The square <b>settings</b>
button turns the keys into settings keys while it is held (page 4).</p>
<p>Visit the Striso forum (forum.striso.com) for questions and discussions on anything Striso related. For the
fork itself, use the issues at %s.</p>
''' % FORK_URL


def page_sounds():
    rows = []
    for bid, eng, rgb in VOICE_KEYS:
        name, what, presets = VOICES[eng]
        rows.append('<tr><td>%s</td><td><b>%s</b><br><span class="dim">%s</span></td><td>%s</td></tr>' % (
            swatch(VOICE_LED[eng]), name, what, ', '.join(presets)))
    return '''
<h2>The sounds</h2>
<p>The on-board synth has seven voice engines. Several are physical models that react to the Striso&rsquo;s
touch the way the real instrument reacts to a player.</p>
<ul class="tight">
<li><b>Bowed strings:</b> a waveguide string driven by a friction bow, with a modelled body. Pressure is the bow,
tilt moves it from fingerboard to bridge, a hard strike gives a short bite; pizzicato included.</li>
<li><b>Woodwinds:</b> modelled reeds and flute (clarinets, saxophones, oboe, bassoon). Pressure is breath; a fast
strike tongues the note.</li>
<li><b>Brass:</b> a lip-reed model with a physical bore, from trumpet to tuba, with harmon and plunger mutes.</li>
<li><b>Electric pianos:</b> physically inspired tines and reeds (Rhodes, Wurlitzer, Pianet and more).</li>
<li><b>Tuned percussion:</b> modal synthesis of real partials (vibraphone, marimba, glockenspiel, bell, kalimba,
wine glass); pressure after the strike rubs and sustains.</li>
<li><b>Virtual analog:</b> saws, sync, a ladder filter and expressive envelopes.</li>
<li><b>strisy:</b> the original Striso sound, the default at power on.</li>
</ul>
<p>All of them play with <b>10-voice polyphony</b> through <b>stereo master effects</b>: chorus, delay, reverb and
tremolo. Every voice responds to pressure, strike velocity, tilt and the pedal. Page 4 explains how to pick a
voice and a preset on the board; the table lists them in the order the up/down LEDs count.</p>
<table class="voices">
<tr><th>LED</th><th>Voice</th><th>Presets</th></tr>
%s
</table>
<p class="caption" style="margin-top:2.5mm">Striso Live (page 7) can shape every parameter of these engines and save your own sounds.</p>
''' % '\n'.join(rows)


def page_settings():
    return '''
<h2>Settings</h2>
<p>While the <b>settings</b> button is held, the keys adjust settings like volume or MIDI mode instead of playing.
The card on page 5 shows which key does what.</p>
<p><b>Switch keys</b> select something: a MIDI mode, a tuning, a preset, a voice. They act as soon as they are pressed,
and the indicator LED lights in the matching colour.</p>
<p><b>Dial keys</b> (dashed ring on the card) hold a value: volume, a sensitivity, a voice&rsquo;s preset. Pressing one
shows its value on the up/down LEDs, from 0 (all off) to 15:</p>
%s
<p>Change the value by nudging the key up or down. For larger changes, make a circular motion with the key:
clockwise for up, anticlockwise for down. Holding a nudge repeats it.</p>
<div class="fig">%s</div>
<p class="note">Settings changed on the board itself are not saved and return to their defaults when the
Striso board is powered up again. Striso Live can save touch settings to the board.</p>

<h3>Voices and presets</h3>
<ol class="tight">
<li>Hold the square <b>settings</b> button.</li>
<li>Press the key of a voice (the <span class="grp" style="color:%s">Voices</span> row on the card). The indicator LED
lights in the voice&rsquo;s colour and the voice plays with the preset it had last.</li>
<li>For another preset, keep the key pressed and <b>nudge it up or down</b>, or <b>circle</b> it. The up/down LEDs
show the preset number, 1 being the first in the table on page 3. Each voice remembers its last preset until power off.</li>
</ol>
<p>The <b>strisy</b> key has no presets and shows no number.</p>
''' % (led_scale_svg(124), gestures_svg(96), GROUP_STROKE[G_VOICE])


def page_card():
    return '''
<div class="card-wrap">%s</div>
<div class="legend">%s</div>
<div class="legend keys">
<span class="lg"><svg width="4mm" height="4mm" viewBox="0 0 10 10"><circle cx="5" cy="5" r="3.2" fill="#fff" stroke="#6b6459" stroke-width="0.6"/></svg>switch key: press</span>
<span class="lg"><svg width="4mm" height="4mm" viewBox="0 0 10 10"><circle cx="5" cy="5" r="3.2" fill="#fff" stroke="#6b6459" stroke-width="0.6"/><circle cx="5" cy="5" r="4.6" fill="none" stroke="#6b6459" stroke-width="0.6" stroke-dasharray="1.2 1.2"/></svg>dial key: nudge or circle</span>
<span class="lg"><svg width="4mm" height="4mm" viewBox="0 0 10 10"><circle cx="5" cy="5" r="2.4" fill="#58c322" stroke="#4a4a4a" stroke-width="0.5"/></svg>colour of the indicator LED</span>
</div>
<p class="caption"><b>What the dials set.</b> MPE: the number of MPE voices (1 to 15). Normal, Mono: the MIDI channel.
Threshold: how light a touch starts a note. Press, velocity, bend, tilt: how strongly the touch counts. Voice keys:
the preset. Tuning keys: a fine tuning offset, also for 12TET. Volume: 0 to 127. Decay: the sustain and decay time.
MPE bend: 12, 24, 48 or 96 semitones. Msg rate: the MIDI message interval, 1 to 15 ms.</p>
<p class="caption"><b>Switches.</b> Presets 1 to 8 load stored settings. Tunings 1 to 8 are named as the factory sets
them. Panic sends all notes off. Send cfg sends the MPE configuration again. <b>Flip 180&deg;</b> turns the layout
around. <b>Reset</b> restarts the board: hold settings, press Reset (red LED), then release settings. Blank keys do
nothing yet.</p>
''' % (card_svg(126), ''.join(
        '<span class="lg"><i style="background:%s;border-color:%s"></i>%s</span>' % (GROUP_FILL[g], GROUP_STROKE[g], g)
        for g, _, _ in GROUPS))


def page_more_settings():
    return '''
<h2>Presets</h2>
<p>There is room for 8 presets to switch quickly between sets of settings. Presets are configured in the
<b>configuration editor</b>: connect the Striso board to a computer with the USB cable while holding the settings
button. The indicator LED blinks red and the Striso shows up as a USB drive; open the file
<span class="mono">CONFIG.HTM</span> on it in your browser. A preset can also be chosen to load at power on.</p>

<h2>Tuning and tuning systems</h2>
<p>To match instruments that are not tuned to A440, nudge or circle the <b>12TET</b> key. Besides the common western
12-tone tuning, several other tuning systems are built in, like 31-tone equal temperament with its pure thirds,
or just intonation where every interval to the C is pure. The tuning keys 1 to 8 load the tunings stored in the
board; the names on the card are the factory ones. They can be customised in the configuration editor.</p>

<h2>Transposing</h2>
<p>The octave up and down buttons can also transpose by other intervals than octaves. Together with the settings
button they transpose a semitone (a minor second in the current tuning) up or down. For a <b>free transpose</b>,
hold both the up and the down button while playing a note, press the key where this note should sound, then
release the first key: all keys move by that interval. Press the settings button together with the octave up
and down buttons to reset any transpose.</p>
<div class="fig">%s</div>

<h2>Reset and factory settings</h2>
<p><b>Restart:</b> hold the settings button, press the bottom-right key (<b>Reset</b> on the card), then release
the settings button. The board restarts with its saved settings.</p>
<p><b>Factory settings:</b> hold the glissando button while plugging in. The board starts with its defaults instead
of the saved power-on preset.</p>
<p><b>Update mode:</b> plugging in with the settings button held starts the board as a USB drive (see <i>Updating
firmware</i>). If you land there by accident, unplug and plug in again.</p>
''' % transpose_svg(88)


def page_live_update():
    return '''
<h2>USB audio</h2>
<p>Connected to a computer, the Striso also appears as a class-compliant USB sound card called <b>&ldquo;Striso
Synth&rdquo;</b>: 48 kHz, stereo, 24 bit (16 bit is offered too), no driver needed on Windows or macOS. Record the
internal synth straight into any DAW, or listen to it through the computer. The headphone jack keeps playing at
the same time, and MIDI over the same USB cable works as before.</p>

<h2>Striso Live, the optional companion app</h2>
<p><b>Striso Live</b> is a free desktop app for Windows and macOS. Nothing needs it; the Striso stays a standalone
instrument. It makes the new sounds easy to explore. With it you can:</p>
<ul class="tight">
<li>turn every sound and effect parameter live while you play;</li>
<li>switch presets and save your own sounds, and A/B them against the preset;</li>
<li>see scales (66 of them), chord names and roman numerals on the board as you play;</li>
<li>set the touch sensitivity and even out individual keys with a per-key equalizer, then save them to the board;</li>
<li>hear the Striso through your computer and record it to WAV;</li>
<li>play full screen in stage mode.</li>
</ul>
<p>Download it from <b>%s</b> (releases: a Windows installer, a Windows zip and a macOS disk image). The README
there also has one-line installers for macOS and Windows.</p>

<h2>Updating firmware</h2>
<ol class="tight">
<li>Plug in the Striso board with the <b>settings</b> (square) button held down. It attaches to the computer as a
flash drive and the indicator LED blinks red.</li>
<li>Keep a backup first: copy <span class="mono">CURRENT.UF2</span> from the drive to your computer.</li>
<li>Copy the new <span class="mono">.uf2</span> file onto the drive. The board reboots by itself.</li>
</ol>
<p>The version on the board is in <span class="mono">INFO_FW.TXT</span>. An update doesn&rsquo;t touch the bootloader
or your saved settings. To go back, copy an official release from <span class="mono">github.com/striso/striso-control-firmware</span>
or your backup the same way.</p>
<p>Fork firmware releases: <b>%s</b>. Build it yourself with <span class="mono">tools/build.sh uf2 VOICE=all</span>;
the README explains the toolchain.</p>
''' % (LIVE_URL, FORK_URL)


def page_trouble():
    return '''
<h2>Troubleshooting</h2>
<dl class="trouble">
<dt>The Striso board is bent, it wobbles on a flat surface.</dt>
<dd>Twist the sides gently with both hands. Try a couple of times until it doesn&rsquo;t wobble anymore.</dd>
<dt>There is a buzzing sound in the audio output.</dt>
<dd>There may be a ground loop if the USB cable is connected to the same device as the audio cable (possibly
indirectly). Power the Striso board from an independent power source, try a better quality audio cable, or use a
ground loop isolator. USB audio avoids the analog cable altogether.</dd>
<dt>The keys are dirty.</dt>
<dd>The keys can be cleaned with a damp cloth. Small particles around the keys can be removed with a toothpick.</dd>
<dt>There are random notes in the MIDI output.</dt>
<dd>Increase the sensitivity <b>threshold</b> a bit. The default is very sensitive, which works well with many
synthesizers that react to velocity and aftertouch, but for some synthesizers and for notation software random
notes are not acceptable.</dd>
<dt>The LED blinks pink.</dt>
<dd>The Striso board receives MIDI messages over USB. If the board behaves strangely, those messages may be the
cause.</dd>
<dt>The sound is gone or very quiet.</dt>
<dd>Check the volume key and the voice keys on the settings card. Starting with factory settings (glissando button
while plugging in) restores the defaults.</dd>
<dt>The computer doesn&rsquo;t show &ldquo;Striso Synth&rdquo;.</dt>
<dd>Use a data USB cable (not a charge-only one) and a direct port. The audio device is part of the fork firmware;
the official firmware has only MIDI.</dd>
<dt>Something else is wrong after the update.</dt>
<dd>Copy your backup or an official release back (see <i>Updating firmware</i>). Nothing in the fork changes the
bootloader.</dd>
</dl>
<div class="credits">
<p>Original guide content &copy; 2023 Piers Titus van der Torren, striso.com. Fork additions &copy; 2026 Aidin
Foroughi. This is a guide for the unofficial fork firmware at %s; it is not a Striso publication. Official
firmware, documentation and support: striso.com and forum.striso.com.</p>
<p>Firmware licensed under the GNU GPL v3 or later. Guide generated by <span class="mono">doc/guide/build.py</span>
from firmware <span class="mono">%s</span> on %s. Typeset in Inter (SIL Open Font License).</p>
</div>
''' % (FORK_URL, git_describe(), datetime.date.today().isoformat())


PAGES = [page_cover, page_thanks, page_sounds, page_settings, page_card, page_more_settings,
         page_live_update, page_trouble]
RUNNING = ['', 'Striso quick start guide', 'The sounds', 'Settings', 'Settings card',
           'Presets, tuning, transposing, reset', 'USB audio, Striso Live, firmware', 'Troubleshooting']

CSS = r'''
@font-face { font-family: 'Inter'; src: url('../Inter.ttf') format('truetype'); font-weight: 100 900; }
:root { --ink: #2a2622; --dim: #6b6459; --accent: #b8921f; --rule: #d9d2c0; }
* { box-sizing: border-box; }
html, body { margin: 0; padding: 0; background: #fff; }
body { font-family: Inter, Helvetica, Arial, sans-serif; color: var(--ink); font-size: 8.6pt; line-height: 1.38;
       font-feature-settings: 'tnum' 1, 'cv11' 1; -webkit-print-color-adjust: exact; print-color-adjust: exact; }
.page { width: 148mm; height: 210mm; position: relative; overflow: hidden; padding: 11mm 11mm 12mm 11mm; }
.page.even { padding-left: 11mm; padding-right: 12mm; }
.page.odd { padding-left: 12mm; padding-right: 11mm; }
h1 { font-size: 20pt; font-weight: 700; letter-spacing: -0.015em; margin: 0; line-height: 1.1; }
h2 { font-size: 11.5pt; font-weight: 700; margin: 0 0 1.4mm; letter-spacing: -0.01em; }
h2 + p, h3 + p { margin-top: 0; }
p + h2, ul + h2, ol + h2, div + h2, table + h2 { margin-top: 4.2mm; }
h3 { font-size: 9.4pt; font-weight: 700; margin: 3.6mm 0 1mm; }
p { margin: 0 0 1.9mm; orphans: 3; widows: 3; }
p.lead { font-size: 9.1pt; margin: 3.5mm 0 3mm; }
.kicker { font-size: 9pt; color: var(--dim); margin-top: 1.2mm; }
.cover-head { display: flex; align-items: center; gap: 5mm; padding-bottom: 2.5mm; border-bottom: 0.3mm solid var(--rule); }
.cover-head .logo { width: 31mm; height: auto; display: block; }
.board { display: block; margin: 1mm auto 2.5mm; }
.caption { font-size: 7.6pt; color: var(--dim); line-height: 1.35; }
.caption b { color: var(--ink); }
.unofficial { position: absolute; left: 12mm; right: 11mm; bottom: 9mm; font-size: 7pt; color: var(--dim);
              border-top: 0.25mm solid var(--rule); padding-top: 1.6mm; }
ul.tight, ol.tight { margin: 0 0 1.9mm; padding-left: 4.5mm; }
ul.tight li, ol.tight li { margin: 0 0 0.9mm; }
.dim { color: var(--dim); }
.mono { font-family: Consolas, 'DejaVu Sans Mono', Menlo, monospace; font-size: 0.93em; }
.note { background: #fdf6dc; border-left: 0.6mm solid var(--accent); padding: 1.5mm 2.5mm; border-radius: 0.6mm; }
.fig { text-align: center; margin: 1.5mm 0 2.5mm; }
.fig svg { display: inline-block; }
table.voices { width: 100%; border-collapse: collapse; font-size: 7.7pt; margin-top: 1mm; }
table.voices th { text-align: left; font-weight: 600; color: var(--dim); border-bottom: 0.3mm solid var(--rule); padding: 0.6mm 1.2mm 0.8mm; font-size: 7.2pt; }
table.voices td { vertical-align: top; padding: 1.1mm 1.2mm; border-bottom: 0.2mm solid #ece7da; }
table.voices td:first-child { width: 6mm; }
table.voices td:nth-child(2) { width: 24mm; white-space: nowrap; }
.sw { display: inline-block; width: 3.4mm; height: 3.4mm; border-radius: 50%; border: 0.3mm solid #4a4a4a; vertical-align: -0.7mm; }
.grp { font-weight: 600; }
.card-wrap { margin: 0 -1mm 2mm; }
.card-wrap svg { display: block; }
.legend { display: flex; flex-wrap: wrap; gap: 1.2mm 4mm; font-size: 7.6pt; margin: 0 0 1.6mm; }
.legend.keys { margin-bottom: 3mm; color: var(--dim); }
.legend.keys svg { vertical-align: -0.9mm; margin-right: 1.2mm; }
.legend .lg i { display: inline-block; width: 3.2mm; height: 3.2mm; border-radius: 50%; border: 0.35mm solid; vertical-align: -0.6mm; margin-right: 1.3mm; }
dl.trouble { margin: 0; }
dl.trouble dt { font-weight: 600; margin-top: 1.6mm; }
dl.trouble dd { margin: 0.2mm 0 0 4mm; position: relative; }
dl.trouble dd::before { content: '\2192'; position: absolute; left: -4mm; color: var(--dim); }
.credits { position: absolute; left: 11mm; right: 11mm; bottom: 10mm; font-size: 7pt; color: var(--dim); line-height: 1.35;
           border-top: 0.25mm solid var(--rule); padding-top: 1.8mm; }
.credits p { margin: 0 0 1mm; }
.folio { position: absolute; bottom: 6mm; font-size: 7pt; color: var(--dim); }
.page.odd .folio { right: 11mm; text-align: right; }
.page.even .folio { left: 11mm; }
.folio span { margin: 0 1.5mm; }
'''

A5_CSS = '@page { size: 148mm 210mm; margin: 0; }\n.page { break-after: page; }\n.page:last-child { break-after: auto; }\n'
A4_CSS = ('@page { size: 297mm 210mm; margin: 0; }\n'
          '.sheet { width: 297mm; height: 210mm; display: flex; position: relative; break-after: page; }\n'
          '.sheet:last-child { break-after: auto; }\n'
          '.sheet .page { flex: none; }\n'
          '.fold { position: absolute; left: 148.5mm; width: 0; border-left: 0.15mm dashed #bbb; height: 5mm; }\n'
          '.fold.top { top: 0; } .fold.bottom { bottom: 0; }\n')


def render_page(n, html):
    cls = 'page ' + ('odd' if n % 2 else 'even')
    folio = ''
    if n > 1:
        if n % 2:
            folio = '<div class="folio">%s<span>&middot;</span>%d</div>' % (esc(RUNNING[n - 1]), n)
        else:
            folio = '<div class="folio">%d<span>&middot;</span>%s</div>' % (n, esc(RUNNING[n - 1]))
    return '<section class="%s" id="p%d">%s%s</section>' % (cls, n, html, folio)


def html_doc(title, css, body):
    return ('<!doctype html>\n<html lang="en"><head><meta charset="utf-8"><title>%s</title>\n'
            '<style>%s\n%s</style></head>\n<body>\n%s\n</body></html>\n' % (esc(title), CSS, css, body))


def build_html():
    os.makedirs(BUILD, exist_ok=True)
    pages = [render_page(i + 1, fn()).replace('href="logo.svg"', 'href="../logo.svg"') for i, fn in enumerate(PAGES)]
    a5 = html_doc('Striso quick start guide (fork firmware)', A5_CSS, '\n'.join(pages))
    # booklet imposition: 8 pages on 2 sheets, printed double sided (flip on the short edge)
    order = [(8, 1), (2, 7), (6, 3), (4, 5)]
    sheets = []
    for left, right in order:
        sheets.append('<div class="sheet"><div class="fold top"></div><div class="fold bottom"></div>%s%s</div>' % (
            pages[left - 1], pages[right - 1]))
    a4 = html_doc('Striso quick start guide (fork firmware), A4 booklet', A4_CSS, '\n'.join(sheets))
    card = card_svg(260, standalone=True, title='Settings', subtitle='expressive synth fork firmware').replace('href="logo.svg"', 'href="../logo.svg"')
    card_html = html_doc('Striso settings card', '@page { size: auto; margin: 0 }\nbody { background: #fff; }\n.card { display:block; }',
                         card.replace('width="260mm"', 'width="1600px"').replace('height="%smm"' % f(260 * 162 / 185), ''))
    for name, text in (('a5.html', a5), ('a4.html', a4), ('card.html', card_html)):
        with open(os.path.join(BUILD, name), 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(text)
    # the standalone SVG, with the wordmark inlined so the file stands on its own
    logo = open(os.path.join(HERE, 'logo.svg'), encoding='utf-8').read()
    logo_path = re.search(r'<path[^>]*/>', logo).group(0)
    logo_vb = re.search(r'viewBox="([^"]*)"', logo).group(1)
    svg = card_svg(185, standalone=True, title='Settings', subtitle='expressive synth fork firmware, ' + git_describe())
    svg = '<?xml version="1.0" encoding="UTF-8"?>\n<!-- Generated by doc/guide/build.py from the firmware tables; do not edit by hand. -->\n' + svg
    svg = re.sub(r'<image href="logo.svg" x="([^"]*)" y="([^"]*)" width="([^"]*)" height="([^"]*)"/>',
                 lambda m: '<svg x="%s" y="%s" width="%s" height="%s" viewBox="%s">%s</svg>' % (
                     m.group(1), m.group(2), m.group(3), m.group(4), logo_vb, logo_path), svg)
    with open(os.path.join(DOC, 'strisoboard-settings.svg'), 'w', encoding='utf-8', newline='\n') as fh:
        fh.write(svg + '\n')
    return a5, a4


def verify_against_firmware():
    """Checks the tables above against synth_control.cpp and voice_*.h, so the guide can't drift."""
    def read(name):
        return open(os.path.join(REPO, name), encoding='utf-8', errors='replace').read()
    src = read('synth_control.cpp')
    new = src[src.index('#else // !OLD_CONFIG_LAYOUT'):src.index('#endif // !OLD_CONFIG_LAYOUT')]
    body = new[new.index('float config_but('):]
    cases = set(int(m) for m in re.findall(r'^\s*case \((\d+)\):', body, re.M))
    if cases != set(SETTINGS_BY_ID):
        sys.exit('settings keys differ from config_but(): %s' % sorted(cases ^ set(SETTINGS_BY_ID)))
    for bid, eng, rgb in VOICE_KEYS:
        m = re.search(r'case \(%d\): return config_voice\("%s", (\d+), (\d+), (\d+),' % (bid, eng), new)
        if not m or tuple(map(int, m.groups())) != tuple(rgb):
            sys.exit('voice key %d (%s) differs from config_voice()' % (bid, eng))
    for eng, hdr, array in (('va', 'voice_va.h', 'names'), ('bowed', 'voice_bowed.h', 'names'),
                            ('brass', 'voice_brass.h', 'names'), ('ep', 'voice_ep.h', 'names'),
                            ('modal', 'voice_modal.h', 'presets'), ('wind', 'voice_wind.h', 'preset_info')):
        h = read(hdr)
        n = int(re.search(r'NUM_PRESETS = (\d+)', h).group(1))
        start = h.index(array + '[NUM_PRESETS] = {') + len(array + '[NUM_PRESETS] = ')
        depth, end = 0, start
        for end in range(start, len(h)):   # the initializer up to its matching brace
            depth += (h[end] == '{') - (h[end] == '}')
            if depth == 0:
                break
        names = re.findall(r'"([^"]+)"', h[start:end])
        if names != VOICES[eng][2] or len(names) != n:
            sys.exit('presets of %s differ from %s: %s' % (eng, hdr, names))
    for n in range(1, 9):
        if ('"sT%dname "' % n) not in read('config.h'):
            sys.exit('tuning %d missing from config.h' % n)
    assert spell(C1[25]) == ('D', 0) and key_octave(25) == 3 and pitch(25) == 0
    assert led_state(0) == [0, 0, 0, 0] and led_state(8) == [15, 15, 15, 15] and led_state(15) == [3, 0, 0, 0]
    print('tables verified against synth_control.cpp, voice_*.h and config.h')


def find_edge():
    for p in (os.environ.get('EDGE'),
              r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',
              r'C:\Program Files\Microsoft\Edge\Application\msedge.exe',
              '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge',
              shutil.which('msedge'), shutil.which('microsoft-edge'), shutil.which('google-chrome'),
              shutil.which('chromium'), shutil.which('chromium-browser')):
        if p and os.path.exists(p):
            return p
    return None


def edge(args, output):
    """Runs Edge headless and waits for `output`. When another Edge is already running, msedge.exe
    hands the job to it and returns at once; the file then appears seconds later, so wait for it."""
    exe = find_edge()
    if not exe:
        sys.exit('No Microsoft Edge or Chromium found; set EDGE=<path to msedge>.')
    if os.path.exists(output):
        os.remove(output)
    # a fresh profile per call: a second call with the same profile would hand its work to the
    # first instance while that one is still shutting down, and produce nothing
    profile = tempfile.mkdtemp(prefix='edge-', dir=BUILD)
    cmd = [exe, '--headless=new', '--disable-gpu', '--no-sandbox', '--hide-scrollbars',
           '--user-data-dir=' + profile, '--no-first-run', '--virtual-time-budget=4000'] + args
    try:
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + 180
        size = -1
        while time.time() < deadline:
            now = os.path.getsize(output) if os.path.exists(output) else -1
            if now > 0 and now == size:
                return
            size = now
            time.sleep(1.0)
        sys.exit('Edge did not write %s within 3 minutes' % output)
    finally:
        shutil.rmtree(profile, ignore_errors=True)


def file_url(path):
    return 'file:///' + os.path.abspath(path).replace('\\', '/')


def build_pdfs():
    for html, pdf in (('a4.html', 'quick_start_guide.pdf'), ('a5.html', 'quick_start_guide_a5.pdf')):
        out = os.path.join(DOC, pdf)
        edge(['--no-pdf-header-footer', '--print-to-pdf=' + out, file_url(os.path.join(BUILD, html))], out)
        print('wrote', os.path.relpath(out, REPO), os.path.getsize(out) // 1024, 'kB')
    png = os.path.join(DOC, 'strisoboard-settings.png')
    edge(['--window-size=1600,1402', '--screenshot=' + png, file_url(os.path.join(BUILD, 'card.html'))], png)
    print('wrote', os.path.relpath(png, REPO), os.path.getsize(png) // 1024, 'kB')


if __name__ == '__main__':
    verify_against_firmware()
    build_html()
    print('wrote', os.path.relpath(os.path.join(DOC, 'strisoboard-settings.svg'), REPO), 'and', os.path.relpath(BUILD, REPO))
    if '--html-only' not in sys.argv:
        build_pdfs()
