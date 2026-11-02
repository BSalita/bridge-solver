# Vendored from ddss

`Par.cpp`, `DealerPar.cpp`, `PBN.cpp` and `PBN.h` are copied unchanged from
ddss (https://github.com/BSalita/ddss, a fork of DDS 2.9, commit 819e22f),
and `../include/dll.h` is ddss's `include/dll.h`. They are DDS's par
calculations and PBN parsing, which work on DD tables and strings only, so the
library reuses them rather than re-implementing them. `dds.h` here is a small
stand-in for ddss's internal header.

DDS is Copyright (C) 2006-2014 by Bo Haglund / 2014-2018 by Bo Haglund & Soren
Hein, under the Apache License 2.0 (`LICENSE-DDS`).
