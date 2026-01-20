# SPDX-License-Identifier: GPL-2.0+
#
# Entry-type module for Rockchip boost binary
#

from binman.etype.blob_named_by_arg import Entry_blob_named_by_arg

class Entry_rockchip_boost(Entry_blob_named_by_arg):
    """Rockchip boost binary

    Properties / Entry arguments:
        - rockchip-boost-path: Filename of file to read into the entry,
                               typically rk3576-boost.bin

    This entry holds an external boost binary used by some Rockchip SoCs
    (like RK3576) to configure CPU frequency boost settings in early boot.
    The boost binary runs in SRAM before the DDR initialization code.
    """
    def __init__(self, section, etype, node):
        super().__init__(section, etype, node, 'rockchip-boost')
        self.external = True
