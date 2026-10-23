# Prints the ChibiOS file lists the firmware uses, as CMake code, straight from ChibiOS' own make
# fragments (with USE_SMART_BUILD the HAL lists depend on cfg/halconf.h). Run by cmake/ChibiOS.cmake:
#   make -s -C <source root> -f cmake/chibios_lists.mk CHIBIOS=<rel> CONFDIR=<rel> BOARDMK=<rel> print
# Paths come out as the fragments write them (relative to the source root).

USE_SMART_BUILD ?= yes

include $(CHIBIOS)/os/license/license.mk
include $(CHIBIOS)/os/common/startup/ARMCMx/compilers/GCC/mk/startup_stm32h7xx.mk
include $(CHIBIOS)/os/hal/hal.mk
include $(CHIBIOS)/os/hal/ports/STM32/STM32H7xx/platform.mk
include $(BOARDMK)
include $(CHIBIOS)/os/hal/osal/rt-nil/osal.mk
include $(CHIBIOS)/os/rt/rt.mk
include $(CHIBIOS)/os/common/ports/ARMCMx/compilers/GCC/mk/port_v7m.mk
include $(CHIBIOS)/tools/mk/autobuild.mk
include $(CHIBIOS)/os/hal/lib/streams/streams.mk

print:
	@echo 'set(CHIBIOS_ALLCSRC $(ALLCSRC))'
	@echo 'set(CHIBIOS_ALLCPPSRC $(ALLCPPSRC))'
	@echo 'set(CHIBIOS_ALLASMSRC $(ALLASMSRC))'
	@echo 'set(CHIBIOS_ALLXASMSRC $(ALLXASMSRC))'
	@echo 'set(CHIBIOS_ALLINC $(ALLINC))'
	@echo 'set(CHIBIOS_STARTUPLD $(STARTUPLD))'
