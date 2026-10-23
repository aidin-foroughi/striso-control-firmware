#!/usr/bin/env python3

import argparse
import re
import subprocess
import os

def touch(fname, times=None):
    with open(fname, 'a'):
        os.utime(fname, times)

def interface2embedded_interface(interface_cpp):
    v_re = re.compile(r'ui_interface->open\w*\("v(_bas|)(\d+)"\);')
    add_re = re.compile(r'ui_interface->add\w*\(("\w*", &\w*),')
    ch_lines = []
    cur_voice = ''
    cur_voice_type = ''
    max_voice = -1
    for line in interface_cpp:
        line = line.strip()
        if line.startswith('ui_interface->open'):
            v = v_re.findall(line)
            if v:
                cur_voice_type = v[0][0]
                cur_voice = v[0][1]
                if int(cur_voice) > max_voice:
                    max_voice = int(cur_voice)
        elif line.startswith('ui_interface->add'):
            par = add_re.findall(line)[0].split(',')
            name = par[0].strip('"')
            loc = par[1].strip()
            if name in ('acc_abs','acc_x','acc_y','acc_z','rot_x','rot_y','rot_z','pedal'):
                ch_lines.append('synth_interface.{} = {};'.format(name, loc))
            elif cur_voice and name in ('note','pres','vpres','but_x','but_y'):
                ch_lines.append('synth_interface{}.{}[{}] = {};'.format(
                    cur_voice_type, name, cur_voice, loc))

    return ch_lines, max_voice

def faust_postprocess(source):

    # disable cmath, gives compile error since ubuntu 15.10
    source = source.replace('#include <cmath>', '//#include <cmath>')

    # remove clone method as it uses the unsupported new operator
    source = source.replace('\tvirtual mydsp* clone() {\n\t\treturn new mydsp();\n\t}', '')

    # replace interface function with simpler version without strings
    start = source.find('virtual void buildUserInterface(UI* ui_interface) {')
    stop = source.find('}',start)
    interface_embedded, max_voice = interface2embedded_interface(source[start:stop].splitlines())

    new_source = ''.join((
        source[:start],
        'virtual void buildUserInterfaceEmbedded() {\n\t\t',
        '\n\t\t'.join(interface_embedded),
        '\n\t',
        source[stop:]))

    return new_source, max_voice

def faust_replace_fixed_sliders(faust_in):
    replace_slider = re.compile(r'hslider\(".*",([^,]*),[^)]*\)')
    return replace_slider.sub(r'\1',faust_in)

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description='Compile synth.dsp with Faust into the firmware synth (synth.cpp).')
    ap.add_argument('--dsp', default=os.path.join(here, 'synth.dsp'))
    ap.add_argument('--template', default=os.path.join(here, 'faust_synth_template.cpp'))
    ap.add_argument('--out', default=os.path.join(here, 'synth.cpp'))
    ap.add_argument('--synth-h', default=os.path.join(here, '..', 'synth.h'))
    ap.add_argument('--tmp-dir', default=None,
                    help='where to write synth.dsp.tmp (default: next to --out)')
    ap.add_argument('--check-voicecount', action='store_true',
                    help='fail if VOICECOUNT in synth.h differs instead of rewriting synth.h (the build uses this)')
    args = ap.parse_args()
    tmp_dir = args.tmp_dir or os.path.dirname(os.path.abspath(args.out))
    os.makedirs(tmp_dir, exist_ok=True)

    # the generated code names its source file: keep it synth.dsp.tmp
    tmp_name = 'synth.dsp.tmp'

    # preprocess: replace hsliders with fixed values, useful for quick testing
    # of settings on the pc but compiling the as fixed on embedded platform.
    # vsliders stay variable.
    with open(os.path.join(tmp_dir, tmp_name), 'w', newline='\n') as f:
        f.write(faust_replace_fixed_sliders(open(args.dsp).read()))

    # run faust compiler (-I: the libraries next to synth.dsp, e.g. fast.lib)
    extra_args = os.environ.get('FAUST_ARGS', '').split()
    faust_cpp = subprocess.check_output(
        ['faust', '-I', os.path.dirname(os.path.abspath(args.dsp)), '-a', os.path.abspath(args.template),
         tmp_name, '-lang', 'ocpp'] + extra_args, cwd=tmp_dir).decode()

    # do some postprocessing for efficient embedded use
    faust_cpp, max_voice = faust_postprocess(faust_cpp)
    with open(args.out, 'w', newline='\n') as f:
        f.write(faust_cpp)

    # change VOICECOUNT in 'synth.h' if different from voicecount in synth.dsp
    synth_h = open(args.synth_h).readlines()
    vc_line = [n for n,line in enumerate(synth_h) if line.startswith('#define VOICECOUNT')][0]
    vc = int(synth_h[vc_line].split()[-1])
    if vc != max_voice + 1:
        if args.check_voicecount:
            raise SystemExit('synth.dsp has {} voices but {} says VOICECOUNT {}: run {} once to update it'.format(
                max_voice + 1, args.synth_h, vc, os.path.basename(__file__)))
        synth_h[vc_line] = '#define VOICECOUNT {}\n'.format(max_voice+1)
        with open(args.synth_h, 'w') as f:
            f.writelines(synth_h)
        # make sure synth_contol gets compiled again
        touch(os.path.join(os.path.dirname(os.path.abspath(args.synth_h)), 'synth_control.cpp'))

if __name__ == '__main__':
    main()
