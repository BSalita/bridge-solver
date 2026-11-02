// Stand-in for ddss's internal src/dds.h: just what the vendored Par.cpp,
// DealerPar.cpp and PBN.cpp use. The tables are defined in dds-api.cc with
// ddss's values (src/Init.cpp).
#ifndef BSC_VENDOR_DDS_H
#define BSC_VENDOR_DDS_H

#include <string.h>

#include "../include/dll.h"

extern unsigned short int bitMapRank[16];
extern unsigned char cardSuit[DDS_STRAINS];

#endif
