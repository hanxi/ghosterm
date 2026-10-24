#include "session-history-wire.h"
#include "session-process-wire.h"
#include <assert.h>
#include <string.h>
int main(int argc,char **argv){
    assert(argc==2);uint8_t data[HSW_BYTES],bad[HSW_BYTES];
    hsw_state s={1048576,100,HSW_COMPLETE},out={0};
    assert(hsw_encode(s,data,sizeof(data)));
    if(!strcmp(argv[1],"valid")){
        static const uint8_t expected[32]={0x48,0x53,0x54,1,0,0,0,0,0,0,0,0,0,0x10,0,0,0,0,0,0,0,0,0,100,0,0,0,0,0,0,0,1};
        assert(!memcmp(data,expected,32));
        for(unsigned state=0;state<=2;state++){
            s=(hsw_state){state?1048576:0,state?100:0,state};
            assert(hsw_encode(s,data,32)&&hsw_decode(data,32,&out));
            assert(out.bytes==s.bytes&&out.lines==s.lines&&out.state==state);
        }
    }else{
        assert(!strcmp(argv[1],"invalid"));
        for(unsigned i=0;i<32;i++){
            if(i>=8&&i<24)continue;
            memcpy(bad,data,32);bad[i]=255;out=(hsw_state){77,88,99};
            assert(!hsw_decode(bad,32,&out));assert(out.bytes==77&&out.lines==88&&out.state==99);
        }
        for(unsigned state=0;state<=2;state++)for(unsigned absent=0;absent<2;absent++){
            memcpy(bad,data,32);pw_put64(bad+24,state);pw_put64(bad+(absent?16:8),0);
            assert(!hsw_decode(bad,32,&out));
        }
        memcpy(bad,data,32);pw_put64(bad+24,((uint64_t)1<<32)|1);assert(!hsw_decode(bad,32,&out));
        for(unsigned n=0;n<34;n++)if(n!=32){assert(!hsw_decode(data,n,&out));assert(!hsw_encode(s,bad,n));}
        assert(!hsw_decode(NULL,32,&out)&&!hsw_decode(data,32,NULL)&&!hsw_encode(s,NULL,32));
        for(unsigned state=0;state<=3;state++){
            s=(hsw_state){state==0?1:0,100,state};memset(bad,0xa5,32);
            assert(!hsw_encode(s,bad,32));for(unsigned i=0;i<32;i++)assert(bad[i]==0xa5);
        }
    }
    return 0;
}
