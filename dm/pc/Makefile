# The NVN replay layer (dm/gfx/nvn: nvn_vk.c and what it needs) built for x86-64 Linux, so PC tools can run the game's real Vulkan replay headless (Mesa lavapipe in WSL,
# or any Vulkan driver the loader finds; VK_INSTANCE_LAYERS / VK_LOADER_* / VK_ICD_FILENAMES apply as for any Vulkan program, DM_VK_DEVICE picks the device).
#   make GAME=<engine title ID> VULKAN_INC=<folder holding vulkan/vulkan.h> [GLSLC=<glslc>]        (run from dm/pc; build.sh finds both)
# Outputs in out/pc-<GAME>/: libnvnvk.a (the replay layer + compat + host stand-ins, pc_host.c), golden_play (the golden-frame player), golden_synth (a frame made
# on the PC through the replay with the recorder on: the player's round-trip test). The Android build's compat header is arm64-only; this one is compat/pc_compat.h.
GAME ?= 01008DB008C2C000
CC ?= gcc
GLSLC ?= glslc
OUT ?= out/pc-$(GAME)
GAME_DIRS := $(shell python3 ../games/game.py dirs $(GAME))
GEN = $(OUT)/gen/game_profile.h
NVN = ../gfx/nvn
CFLAGS = -O2 -g -fPIC -MMD -MP -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-format -Wno-misleading-indentation -fno-omit-frame-pointer \
         -include compat/pc_compat.h -Icompat $(if $(VULKAN_INC),-I$(VULKAN_INC)) -I$(NVN) -I$(NVN)/include -I$(OUT)/gen $(addprefix -I,$(GAME_DIRS))
LIBSRC = $(NVN)/nvn_vk.c $(NVN)/nvn_spv.c $(NVN)/nvn_tex.c $(NVN)/nvn_dirty.c $(NVN)/framecap_zip.c $(NVN)/golden_rec.c \
         $(filter-out ../gfx/tex/nvn_fmt_mtl.c,$(wildcard ../gfx/tex/*.c)) pc_host.c
LIBOBJ = $(patsubst %.c,$(OUT)/obj/%.o,$(notdir $(LIBSRC)))
LIBS = -lz -lm -ldl -lpthread
vpath %.c $(NVN) ../gfx/tex . ../gfx/golden

all: $(OUT)/libnvnvk.a $(OUT)/golden_play $(OUT)/golden_synth
$(GEN): $(wildcard ../games/*/game.tsv) ../games/game.py
	@mkdir -p $(dir $@); python3 ../games/game.py header $(GAME) $@
$(OUT)/obj/%.o: %.c $(GEN)
	@mkdir -p $(dir $@); $(CC) $(CFLAGS) -c $< -o $@
$(OUT)/obj/nvn_vk.o: $(NVN)/golden_vk.inc
$(OUT)/libnvnvk.a: $(LIBOBJ)
	rm -f $@; ar rcs $@ $^
$(OUT)/golden_play: $(OUT)/obj/golden_play.o $(OUT)/libnvnvk.a
	$(CC) -o $@ $^ $(LIBS)
# The synthetic frame's shaders: GLSL compiled at build time (SPIR-V 1.0, Vulkan 1.0 environment); their reflection (.vkr) is written by golden_synth itself
SYNTH_SH = $(patsubst ../gfx/golden/synth/%,$(OUT)/synth/%.spv,$(wildcard ../gfx/golden/synth/*.vert ../gfx/golden/synth/*.frag ../gfx/golden/synth/*.comp))
$(OUT)/synth/%.spv: ../gfx/golden/synth/%
	@mkdir -p $(dir $@); $(GLSLC) --target-env=vulkan1.0 -o $@ $<
$(OUT)/golden_synth: $(OUT)/obj/golden_synth.o $(OUT)/libnvnvk.a $(SYNTH_SH)
	$(CC) -o $@ $(OUT)/obj/golden_synth.o $(OUT)/libnvnvk.a $(LIBS)
clean:
	rm -rf $(OUT)
-include $(wildcard $(OUT)/obj/*.d)
.PHONY: all clean
