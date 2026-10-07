#!/usr/bin/env python3
"""Bake a font's tabular figures (OpenType `tnum`) into its default cmap.

lv_font_conv ignores OpenType features, so without this Inter's digits come
out proportional and a live number like "11.1" -> "18.8" visibly shifts as it
updates. After freezing, every digit has the same advance width.

    python3 tools/freeze_tnum.py Inter-Medium.ttf Inter-Medium-tnum.ttf

Needs fontTools (`pip install fonttools`).
"""
import sys

from fontTools.ttLib import TTFont


def main(src: str, dst: str) -> None:
    font = TTFont(src)
    gsub = font["GSUB"].table
    subst = {}
    for rec in gsub.FeatureList.FeatureRecord:
        if rec.FeatureTag != "tnum":
            continue
        for idx in rec.Feature.LookupListIndex:
            lookup = gsub.LookupList.Lookup[idx]
            for sub in lookup.SubTable:
                if lookup.LookupType == 7:  # extension wrapper
                    sub = sub.ExtSubTable
                if hasattr(sub, "mapping"):
                    subst.update(sub.mapping)

    for table in font["cmap"].tables:
        for cp, glyph in list(table.cmap.items()):
            if glyph in subst:
                table.cmap[cp] = subst[glyph]
    font.save(dst)

    hmtx = font["hmtx"]
    widths = {hmtx[font.getBestCmap()[ord(c)]][0] for c in "0123456789"}
    if len(widths) != 1:
        sys.exit(f"{src}: digits still proportional after freeze: {sorted(widths)}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
