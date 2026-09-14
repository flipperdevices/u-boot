# SPDX-License-Identifier: GPL-2.0+
#
# (C) Copyright 2000-2006
# Wolfgang Denk, DENX Software Engineering, wd@denx.de.

obj-y += zlib.o

# These only run once an image is being unpacked, which on some platforms is
# late enough that unaligned accesses are safe. Where the platform says so,
# let the compiler use them: assembling a 64-bit read out of bytes is both
# slower and larger.
ifdef CONFIG_$(PHASE_)SAFE_UNALIGNED_ACCESS
ccflags-remove-y += $(PF_NO_UNALIGNED)
endif
