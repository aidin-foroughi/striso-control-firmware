##############################################################################
# Thin wrapper around the CMake build (CMakeLists.txt, CMakePresets.json; see README "Building").
# Needs cmake >= 3.25, ninja and the arm-none-eabi toolchain on PATH (tools/build.sh builds in
# Docker instead and needs neither).
#
#   make [VOICE=strisy|all|<name>] [SYNTH_ENGINE=strisy|faust]   firmware: build/firmware/*.uf2
#   make test                                                    host unit tests (ctest)
#   make clean | version | release | prog_uf2 | prog_dfu | prog_openocd | prog_uart | prog_bmp
#   make gdb | gdb_bmp | openocd
#
# Every run configures from its arguments, so a VOICE left out goes back to strisy, as before.
##############################################################################

PRESET       ?= firmware
VOICE        ?= strisy
SYNTH_ENGINE ?= strisy
BUILDDIR     := build/$(PRESET)
PROJECT      := striso_control
ELF          := $(BUILDDIR)/$(PROJECT).elf
CMAKE_ARGS   ?=

.PHONY: uf2 all configure test clean version release prog_uf2 prog_dfu prog_openocd prog_uart \
        prog_bmp gdb gdb_bmp openocd

uf2 all: configure
	cmake --build --preset $(PRESET)

configure:
	cmake --preset $(PRESET) -DSTRISO_VOICE=$(VOICE) -DSTRISO_SYNTH_ENGINE=$(SYNTH_ENGINE) $(CMAKE_ARGS)

test:
	cmake --preset host && cmake --build --preset host && ctest --preset host

clean:
	rm -rf $(BUILDDIR)

version: configure
	@grep -o '"[^"]*"' $(BUILDDIR)/generated/fwversion.h 2>/dev/null || cmake --build --preset $(PRESET) --target striso_fwversion

release: uf2
	mkdir -p releases
	cp $(BUILDDIR)/$(PROJECT).uf2 releases/$(PROJECT)_$(shell sed -n 's/.*"\(.*\)".*/\1/p' $(BUILDDIR)/generated/fwversion.h).uf2

prog_uf2: uf2
	@# first put striso in UF2 mode if it isn't (the || true ignores striso_util failure)
	@./tools/utils/striso_util -B 2>/dev/null && sleep 5 || true
	@# uf2conv.py without -c: writes the .uf2 to the bootloader drive it finds
	python3 third_party/uf2/utils/uf2conv.py -f 0xa21e1295 -b 0x08040000 $(BUILDDIR)/$(PROJECT).bin -o $(BUILDDIR)/$(PROJECT).uf2

prog_dfu: uf2
	@# first put striso in DFU mode if it isn't (the || true ignores striso_util failure)
	@./tools/utils/striso_util -d 2>/dev/null && echo Resetting Striso in DFU mode... && sleep 3 || true
	dfu-util -d0483:df11 -a0 -s0x8040000:leave -D $(BUILDDIR)/$(PROJECT).bin

prog_openocd: uf2
	openocd -f interface/stlink.cfg -f target/stm32h7x.cfg -c "program $(ELF) reset exit"

prog_uart: uf2
	./tools/stm32loader.py -e -w -v $(BUILDDIR)/$(PROJECT).bin

# Upload firmware with Black Magic Probe
prog_bmp: uf2
	gdb-multiarch --batch $(ELF) \
		-ex "target extended-remote /dev/ttyACM0" \
		-ex "monitor swdp_scan" \
		-ex "attach 1" \
		-ex "load" \
		-ex "compare-sections" \
		-ex "kill"

# Launch GDB via openocd debugger
gdb:
	gdb-multiarch $(ELF) -ex "tar extended-remote | openocd -f interface/stlink.cfg -f target/stm32h7x.cfg -c \"stm32h7x.cpu configure -rtos auto; gdb_port pipe; log_output openocd.log\""

gdb_bmp:
	gdb-multiarch $(ELF) \
		-ex "target extended-remote /dev/ttyACM0" \
		-ex "monitor swdp_scan" \
		-ex "attach 1"

# Start GDB server for external use (e.g. Eclipse)
openocd:
	openocd -f interface/stlink.cfg -f target/stm32h7x.cfg -c "stm32h7x.cpu configure -rtos auto;"
