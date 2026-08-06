# SPDX-License-Identifier: GPL-2.0+
# SPDX-FileCopyrightText: 2026 Flipper FZCO
#
# Entry-type module for a Linux initramfs/initrd binary blob

from binman.entry import EntryArg
from binman.etype.blob_named_by_arg import Entry_blob_named_by_arg
from dtoc import fdt_util


class Entry_linux_initrd(Entry_blob_named_by_arg):
    """Linux initramfs (initrd) image blob

    Properties / Entry arguments:
        - linux-initrd-path: Filename of file to read into entry. This is
            typically a compressed cpio archive, e.g. initramfs.cpio.gz

    This entry holds the initial ramdisk which the operating system starts
    with, before any other filesystem is available. See :ref:`falcon-mode` for
    using one with an OS started by SPL.

    It is optional: when no path is supplied, neither as the entry argument,
    nor as a `linux-initrd-path` or `filename` property, the entry is dropped
    as absent, without the need for an `optional` property. If this is part of
    a FIT image generation, the image node is still generated, with an empty
    'data' property, which SPL skips with a warning. A path which is supplied
    but not found is reported as a missing blob, as usual.
    """

    def __init__(self, section, etype, node):
        super().__init__(section, etype, node, 'linux-initrd', required=False)
        self.external = True
        path, = self.GetEntryArgsOrProps([EntryArg('linux-initrd-path', str)])
        # The build passes the argument even when LINUX_INITRD is not set,
        # and an empty one hides the property of the same name
        if not path:
            path = fdt_util.GetString(node, 'linux-initrd-path')
            if path:
                self._filename = path
        self._have_path = bool(path) or 'filename' in node.props

    def ObtainContents(self, fake_size=0):
        # Without a path there is nothing to look up, which must not fall back
        # to a file which happens to be called linux-initrd
        if not self._have_path:
            self.optional = True
            self.mark_absent('no initramfs supplied')
            self.SetContents(b'')
            return True
        return super().ObtainContents(fake_size)
