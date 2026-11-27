# Texture sub-module unit tests (seconds): make run
CFLAGS = -O2 -g -Wall -Wextra
t_tex: t_tex.c ../nvn_deswizzle.c ../nvn_deswizzle.h ../../nvn/nvn_tex.c ../../nvn/nvn_tex.h
	clang $(CFLAGS) -o $@ t_tex.c ../nvn_deswizzle.c ../../nvn/nvn_tex.c -lm
t_deswizzle_eq: t_deswizzle_eq.c ../nvn_deswizzle.c ../nvn_deswizzle.h ../../nvn/nvn_tex.c ../../nvn/nvn_tex.h
	clang $(CFLAGS) -o $@ t_deswizzle_eq.c ../nvn_deswizzle.c ../../nvn/nvn_tex.c -lm
t_fmt_mtl: t_fmt_mtl.m ../nvn_fmt_mtl.c ../nvn_fmt_mtl.h
	clang $(CFLAGS) -fobjc-arc -o $@ t_fmt_mtl.m ../nvn_fmt_mtl.c -framework Metal -framework Foundation
# R11G11B10F <-> RGBA16F (../b10_conv.h, the DM_VK_B10_AS_RGBA16 stand-in): every channel code, every half (any C compiler, no Metal: make t_b10 && ./t_b10)
t_b10: t_b10.c ../b10_conv.h
	$(CC) $(CFLAGS) -o $@ t_b10.c -lm
run: t_tex t_fmt_mtl t_deswizzle_eq t_b10
	./t_fmt_mtl && ./t_tex && ./t_deswizzle_eq && ./t_b10
.PHONY: run
