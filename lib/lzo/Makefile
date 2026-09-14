# SPDX-License-Identifier: GPL-2.0+
#
# (C) Copyright 2008
# Stefan Roese, DENX Software Engineering, sr@denx.de.

obj-y += lzo1x_decompress.o

# These only run once an image is being unpacked, which on some platforms is
# late enough that unaligned accesses are safe. Where the platform says so,
# let the compiler use them: assembling a 64-bit read out of bytes is both
# slower and larger.
ifdef CONFIG_$(PHASE_)SAFE_UNALIGNED_ACCESS
ccflags-remove-y += $(PF_NO_UNALIGNED)
endif
