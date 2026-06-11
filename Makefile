# cordic-math — host test / vector build
#
# Bit-equality between host and device requires identical IEEE-754 single
# precision behavior, hence on BOTH sides:
#   -ffp-contract=off   no FMA contraction (M4F has VFMA, x86 has FMA3)
#   no -ffast-math / -funsafe-math-optimizations
#   -fno-builtin        compiler must not substitute its own math knowledge
# and on the device: FPSCR left at reset (round-to-nearest, flush-to-zero
# DISABLED).

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
CFLAGS  += -ffp-contract=off -fno-builtin
INC      = -Iinclude -Isrc

BUILD    = build

# Library objects for the host build: public symbols prefixed cm_* so the
# system libm remains available as the test reference.
LIBOBJS  = $(BUILD)/cordic_math.o $(BUILD)/math_emul.o

all: test

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c src/cordic_port.h include/math.h | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -DCORDIC_MATH_PREFIX -c $< -o $@

# test_math.c uses the SYSTEM <math.h>; only extern cm_* declarations.
$(BUILD)/test_math: test/test_math.c $(LIBOBJS)
	$(CC) $(CFLAGS) $^ -lm -o $@

# vector dumper, host flavor (emulated backend)
$(BUILD)/dump_emul: test/device_dump.c $(BUILD)/math_emul.o
	$(CC) $(CFLAGS) $(INC) $^ -o $@

test: $(BUILD)/test_math
	./$(BUILD)/test_math

vectors: $(BUILD)/dump_emul
	./$(BUILD)/dump_emul > $(BUILD)/vectors_emul.txt
	@echo "wrote $(BUILD)/vectors_emul.txt — diff against the device capture"

clean:
	rm -rf $(BUILD)

.PHONY: all test vectors clean

# ---------------------------------------------------------------------------
# Device build (reference; integrate into the cnav build system)
#
#   sources:  src/cordic_math.c src/math_stm32.c
#   include:  -Iinclude -Isrc        (include/ must precede the toolchain's
#                                     own headers so math.h resolves here)
#   flags:    -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard
#             -ffp-contract=off -fno-builtin -O2
#
#   The application must enable the CORDIC peripheral clock (RCC AHB1,
#   CORDICEN) before the first math call; the library does no clock
#   management.
#
#   Silicon calibration capture (one-time):
#     compile test/device_dump.c (without CM_DUMP_NO_MAIN it provides
#     main(); retarget printf or define CM_DUMP_PRINTF) together with
#     src/math_stm32.c, run on the target, capture the text output, then:
#       make vectors && diff build/vectors_emul.txt device_capture.txt
# ---------------------------------------------------------------------------
