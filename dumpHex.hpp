

#ifndef __DUMPHEX_H__
#define __DUMPHEX_H__

void dumpHex(std::ostream &outs, const uint8_t *b, uint32_t l, uint32_t p = -1);
struct HexDump{
    const uint8_t * v;
    uint32_t l;     
    uint32_t p = -1;
};                  
                    
std::ostream &operator << (std::ostream&s,HexDump h);

#endif
