"""The configuration schema (src/app/config_schema.hpp) for the Python tools.

Reads the firmware's schema header, the one source of the config keys, and gives
- the default records, byte for byte as the firmware's default_config (config_table.hpp builds
  them the same way; tests/test_config_schema.py checks both against tests/golden/firmware_bytes.txt),
- the key names and the checks of a record (tools/striso.py),
- the config editor's key list, its `doc` and `deprecated` tables (config_editor/config_editor.py).

  config_schema.py              print the default records
  config_schema.py --editor-js  print the editor's tables
"""
import ast
import json
import re
import sys
from pathlib import Path

SCHEMA = Path(__file__).resolve().parent.parent / "src/app/config_schema.hpp"

TYPES = "sifh"  # text, integer, decimal, hexadecimal
_TOKEN = re.compile(r"""\s*(?:("[^"\\]*")|('.')|(0x[0-9a-fA-F]+|-?\d+(?:\.\d*)?)|(true|false)|(\{)|(\})|(,)|(\.\w+)\s*=|(//[^\n]*))""")


class Field:
    """a config_schema.hpp Field, with its defaults"""
    DEFAULTS = dict(value="", text="", help="", options="", lo=0, hi=-1, per_key=False, deprecated=False)

    def __init__(self, type, name, **kw):
        unknown = set(kw) - set(self.DEFAULTS)
        if unknown:
            raise ValueError(f"config_schema.hpp: unknown Field members {unknown}")
        self.type, self.name = type, name
        for k, v in self.DEFAULTS.items():
            setattr(self, k, kw.get(k, v))


def _tokens(s):
    """The literals, braces and designators of an initializer, comments dropped."""
    out, i = [], 0
    while i < len(s):
        if s[i:].strip() == "":
            break
        m = _TOKEN.match(s, i)
        if not m:
            raise ValueError(f"config_schema.hpp: can't read {s[i:i + 40]!r}")
        i = m.end()
        st, ch, num, b, lb, rb, comma, desig, comment = m.groups()
        if st is not None:
            out.append(("lit", ast.literal_eval(st)))
        elif ch is not None:
            out.append(("lit", ch[1]))
        elif num is not None:
            out.append(("lit", int(num, 16) if num.startswith("0x") else float(num) if "." in num else int(num)))
        elif b is not None:
            out.append(("lit", b == "true"))
        elif lb or rb:
            out.append((lb or rb, None))
        elif desig is not None:
            out.append(("desig", desig[1:]))
    return out


def _braced(tokens):
    """Nested lists of the literals of a braced initializer ({{"a", "b"}, {}} -> [["a", "b"], []]);
    designated entries become dicts."""
    pos = 0

    def group():
        nonlocal pos
        assert tokens[pos][0] == "{"
        pos += 1
        items, named, desig = [], {}, None
        while tokens[pos][0] != "}":
            kind, v = tokens[pos]
            if kind == "{":
                val = group()
            elif kind == "desig":
                desig = v
                pos += 1
                continue
            else:
                val = v
                pos += 1
            if desig is not None:
                named[desig] = val
                desig = None
            else:
                items.append(val)
        pos += 1
        return named if named else items
    return group()


class Schema:
    def __init__(self, path=SCHEMA):
        src = Path(path).read_text()
        self.consts = {}
        for name, value in re.findall(r"inline constexpr (?:unsigned|int|const char\*) (k\w+) = ([^;]+);", src):
            self.consts[name] = _tokens(value)[0][1]
        tables = {}
        for name, body in re.findall(r"inline constexpr (?:Field|const char\*) (k\w+)(?:\[[\w ]*\])+ = (\{.*?\n\});", src, re.S):
            tables[name] = _braced([t for t in _tokens(body) if t[0] != ","])
        self.general = [Field(**f) for f in tables["kGeneral"]]
        self.preset = [Field(**f) for f in tables["kPreset"]]
        self.tuning = [Field(**f) for f in tables["kTuning"]]
        self.calibration = [Field(**f) for f in tables["kCalibration"]]
        self.preset_values = tables["kPresetValues"]
        self.tuning_values = tables["kTuningValues"]
        self.tuning_notes = tables["kTuningNotes"]
        c = self.consts
        self.flash_address, self.flash_size = c["kFlashAddress"], c["kFlashSize"]
        self.header = (c["kHeaderKey"], c["kHeaderValue"])
        self.end = (c["kEndKey"], c["kEndValue"])
        self.keys, self.note_names = c["kKeys"], c["kNoteNames"]
        assert len(self.preset_values) == c["kPresets"] and len(self.tuning_values) == c["kTunings"]
        self._records = self._build()
        self._key_type = {k: k[0] for k, _ in self._records[1:-1]}
        for f in self.general:
            if f.deprecated:
                self._key_type[self._pad(f.type + "G" + f.name)] = f.type

    # --- keys
    def key_name(self, n):
        """key number n (0..60): its name, F_1 .. B_4 (config_store.cpp put_button_name)"""
        note, octave = (n + 7) % 17, (n + 7) // 17
        return self.note_names[2 * note:2 * note + 2] + chr(ord("1") + octave)

    @staticmethod
    def _pad(s):
        if len(s) > 8:
            raise ValueError(f"config key or value longer than 8 characters: {s!r}")
        return s.ljust(8)

    def _field(self, fields, name, what):
        for f in fields:
            if f.name == name.rstrip():
                return f
        raise KeyError(f"no {what} setting {name!r} in config_schema.hpp")

    def general_key(self, name):
        f = self._field(self.general, name, "general")
        return self._pad(f.type + "G" + f.name)

    def preset_key(self, n, name):
        f = self._field(self.preset, name, "preset")
        return self._pad(f"{f.type}P{n}{f.name}")

    def tuning_key(self, n, name):
        f = self._field(self.tuning, name, "tuning")
        return self._pad(f"{f.type}T{n}{f.name}")

    def calibration_key(self, name, key_name):
        f = self._field(self.calibration, name, "calibration")
        if key_name not in [self.key_name(n) for n in range(self.keys)]:
            raise KeyError(f"no key named {key_name!r}")
        return self._pad(f"{f.type}C{f.name}{key_name}")

    # --- the default records (config_table.hpp make_defaults: the same order and rules)
    def _build(self):
        recs = [self.header]

        def add(key, value):
            recs.append((self._pad(key), self._pad(value)))

        def fields(prefix, fs, value_of):
            for i, f in enumerate(fs):
                if f.deprecated:
                    continue
                if f.per_key:
                    for n in range(self.keys):
                        add(f.type + prefix + f.name + self.key_name(n), value_of(i, f, n))
                else:
                    add(f.type + prefix + f.name, value_of(i, f, None))

        fields("G", self.general, lambda i, f, n: f.value)
        for p, own in enumerate(self.preset_values, 1):
            fields(f"P{p}", self.preset, lambda i, f, n: own[i] if i < len(own) else f.value if p == 1 else "")
        for t, (own, notes) in enumerate(zip(self.tuning_values, self.tuning_notes), 1):
            fields(f"T{t}", self.tuning, lambda i, f, n: own[i] if i < len(own) else notes[(n + 7) % 17] if notes else f.value)
        fields("C", self.calibration, lambda i, f, n: f.value)
        recs.append(self.end)
        return recs

    def default_records(self):
        """[(key, value)], 8 characters each, from the header to the end record"""
        return list(self._records)

    def default_bytes(self):
        return b"".join(k.encode("ascii") + v.encode("ascii") for k, v in self._records)

    # --- checks
    def check_record(self, key, value):
        """None if (key, value) is a setting of the firmware with a value of its type, else why not"""
        try:
            key, value = self._pad(key), self._pad(value)
        except ValueError as e:
            return str(e)
        t = self._key_type.get(key)
        if t is None:
            return f"unknown config key {key!r}"
        v = value.rstrip(" ")
        if v == "":
            return None  # blank: not set
        ok = {"s": lambda: all(32 <= ord(c) < 127 for c in value),
              "i": lambda: re.fullmatch(r"[-+]?\d+", v),
              "f": lambda: re.fullmatch(r"[-+]?(\d+\.?\d*|\.\d+)", v),
              "h": lambda: re.fullmatch(r"(#|0x)?[0-9a-fA-F]+", v)}[t]()
        return None if ok else f"{key.strip()}: {value!r} is not a{'n' if t in 'ih' else ''} {dict(s='text', i='integer', f='decimal', h='hexadecimal')[t]} value"

    # --- the config editor
    def editor_js(self):
        """The editor's `deprecated` and `doc` tables (keys with x for the preset/tuning number)."""
        dep = [self._pad(f.type + "G" + f.name) for f in self.general if f.deprecated]
        out = ["const deprecated = {" + ", ".join(f"{json.dumps(k)}:true" for k in dep) + "};", "const doc = {"]
        for prefix, fs in (("G", self.general), ("Tx", self.tuning), ("Px", self.preset)):
            for f in fs:
                if not f.text:
                    continue
                parts = [f"text:{json.dumps(f.text)}"]
                if f.help:
                    parts.append(f"help:{json.dumps(f.help)}")
                if f.options:
                    parts.append("options:[" + ", ".join(json.dumps(o) for o in f.options.split("|")) + "]")
                if f.lo <= f.hi:
                    parts.append(f"check:function(x) {{return clamp(x, {f.lo:g}, {f.hi:g});}}")
                key = f.type + prefix + f.name
                out.append(f"  {(key + ':').ljust(9)} {{{', '.join(parts)}}},")
        out.append("}")
        return "\n".join(out) + "\n"


def main(argv):
    s = Schema()
    if argv[:1] == ["--editor-js"]:
        sys.stdout.write(s.editor_js())
    else:
        for k, v in s.default_records():
            print(f"{k!r} {v!r}")


if __name__ == "__main__":
    main(sys.argv[1:])
