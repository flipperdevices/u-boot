# SPDX-License-Identifier: GPL-2.0+
# Copyright (c) 2026 Flipper FZCO
#
# Entry-type module for a Linux initramfs/initrd binary blob

from binman.etype.blob_named_by_arg import Entry_blob_named_by_arg


class Entry_linux_initrd(Entry_blob_named_by_arg):
    """Linux initramfs (initrd) image blob

    Properties / Entry arguments:
        - linux-initrd-path: Filename of file to read into entry. This is
            typically a compressed cpio archive (e.g. initramfs.cpio.gz).

    This entry allows binman FIT templates to consume an initramfs provided via
    a make variable, similar to how the kernel is passed to linux-kernel. It is
    optional: when no path is supplied the entry is simply dropped.
    """

    def __init__(self, section, etype, node):
        super().__init__(section, etype, node, 'linux-initrd', required=False)
        self.external = True
