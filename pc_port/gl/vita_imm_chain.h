#pragma once
#include <stdint.h>
// Immediate-path fold chain state. Translator thread only.
struct VitaImmChain {
    uint8_t  armed;
    uint8_t  reg0Ok, reg1Ok;       // TEVREG0/1 writes that keep the key
    uint8_t  mat0Ok, amb0Ok;       // channel-0 material/ambient writes that keep the key
    uint8_t  matFold, which;       // material fold; register folds (bit 0 reg0, bit 1 reg1)
    uint8_t  rgbConst, aConst;
    uint8_t  pad[3];
    uint32_t mode, serial;         // batch GL mode and serial
    uint64_t key;                  // batch key
};
extern VitaImmChain g_vitaImmChain;
