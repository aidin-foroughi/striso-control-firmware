Warm-start seed table (dsp/include/striso_dsp/trumpet_seed.h) for voice_trumpet.h.

In a dev container with the scratch folder (these files, compiled) at /w and the checkout at /src:

    g++ -O2 -std=c++20 -I/src/dsp/include -I/src/tools/harness cap.cpp -o cap
    python capall.py caps.txt 0,1,2,3,4,5,6,7,8 40 82      # CONTAINER=<name>; ~3 s on 32 cores
    python gen_seed.py ../../../dsp/include/striso_dsp/trumpet_seed.h caps.txt

cap.cpp runs each note at the table's breaths (DYN), from a natural tongued start, continues from a
neighbouring breath where that doesn't reach a steady regime at the right pitch, then refines each
state by starting from it once more (the state a seeded note itself settles in). Recapture after any
change to the lips, the bore, the calibration or the presets' sound parameters.
