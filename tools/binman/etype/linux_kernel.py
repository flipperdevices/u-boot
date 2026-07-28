# SPDX-License-Identifier: GPL-2.0+
# Copyright (c) 2026 Flipper FZCO
#
# Entry-type module for Linux kernel binary blob

from binman.etype.blob_named_by_arg import Entry_blob_named_by_arg


class Entry_linux_kernel(Entry_blob_named_by_arg):
    """Linux kernel image blob

    Properties / Entry arguments:
        - linux-kernel-path: Filename of file to read into entry. This is
            typically an uncompressed ARM64 Image.

    This entry allows binman FIT templates to consume a kernel provided via
    make variable, similar to how BL31 is passed to atf-bl31.
    """

    def __init__(self, section, etype, node):
        super().__init__(section, etype, node, 'linux-kernel')
        self.external = True
