#!/usr/bin/env python3
"""Embed the config editor page (config.htm) in the firmware as C arrays, split at the
{FWVERSION} and {CONFIGDATA} placeholders the firmware fills in.

usage: config_editor.py [config.htm] [config_editor.h]
(defaults: config.htm next to this script, config_editor.h next to it; the build writes the
header into its own directory, build/<preset>/generated/config_editor/)"""
import os
import sys


def convert_to_carray(file_content, var_name):
    outp = "const unsigned long %s_len = %d;\n" % (var_name, len(file_content))
    outp += "const unsigned char %s[] __attribute__((aligned(16))) = {" % var_name
    for i in range(len(file_content)):
        if i % 16 == 0:
            outp += "\n"
        outp += "0x%02x, " % file_content[i]
    outp += "\n};\n"
    return outp


here = os.path.dirname(os.path.abspath(__file__))
src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "config.htm")
dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "config_editor.h")

with open(src, mode='rb') as f:
    inpbuf = f.read()

p1, remainder = inpbuf.split(b"{FWVERSION}")
p2, p3 = remainder.split(b"{CONFIGDATA}")
outpbuf = convert_to_carray(p1, "confightm_p1")
outpbuf += convert_to_carray(p2, "confightm_p2")
outpbuf += convert_to_carray(p3, "confightm_p3")

os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
# newline='\n': the same bytes on every platform
with open(dst, mode='w', newline='\n') as f:
    f.write(outpbuf)
