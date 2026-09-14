# SPDX-License-Identifier: GPL-2.0+
#
# Copyright (C) 2007-2008 Industrie Dial Face S.p.A.
# Luigi 'Comio' Mantellini (luigi.mantellini@idf-hit.com)
#
# (C) Copyright 2003-2006
# Wolfgang Denk, DENX Software Engineering, wd@denx.de.

ccflags-y += -DZ7_LZMA_PROB32
ccflags-$(CONFIG_$(PHASE_)LZMA_SIZE_OPTIMIZATION) += -DZ7_LZMA_SIZE_OPT

obj-y += LzmaDec.o LzmaTools.o

# These only run once an image is being unpacked, which on some platforms is
# late enough that unaligned accesses are safe. Where the platform says so,
# let the compiler use them: assembling a 64-bit read out of bytes is both
# slower and larger.
ifdef CONFIG_$(PHASE_)SAFE_UNALIGNED_ACCESS
ccflags-remove-y += $(PF_NO_UNALIGNED)
endif
