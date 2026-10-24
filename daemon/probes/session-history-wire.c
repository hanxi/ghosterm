#include "session-history-wire.h"
#include "session-process-wire.h"
#include <string.h>
static bool valid(hsw_state s){return s.state<=HSW_TRUNCATED && (s.state==HSW_UNKNOWN?(!s.bytes&&!s.lines):(s.bytes&&s.lines));}
bool hsw_encode(hsw_state s,void *out,size_t n){
    if(!out || n!=HSW_BYTES || !valid(s))return false;
    uint8_t data[HSW_BYTES]={0x48,0x53,0x54,1};
    pw_put64(data+8,s.bytes);pw_put64(data+16,s.lines);pw_put64(data+24,s.state);
    memcpy(out,data,sizeof(data));return true;
}
bool hsw_decode(const void *data,size_t n,hsw_state *out){
    if(!data || !out || n!=HSW_BYTES)return false;
    const uint8_t *p=data;static const uint8_t header[8]={0x48,0x53,0x54,1};
    if(memcmp(p,header,sizeof(header)))return false;
    uint64_t state=pw_get64(p+24);if(state>HSW_TRUNCATED)return false;
    hsw_state s={pw_get64(p+8),pw_get64(p+16),(unsigned)state};
    if(!valid(s))return false;
    *out=s;return true;
}
