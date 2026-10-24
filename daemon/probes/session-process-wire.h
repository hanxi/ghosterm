/* P0 experiment only. Private inherited socketpair; NOT production IPC v1.
 * No listener, discovery, authentication, tokens or public ABI. */
#ifndef GT_PROCESS_PROBE_WIRE_H
#define GT_PROCESS_PROBE_WIRE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
enum {PW_CHUNK=16384, PW_LIMIT=16*1024*1024, PW_STATE_OUTPUT_SUMMARY=3, PW_STATE_HISTORY=5};
enum {PW_SNAPSHOT=1,PW_CANONICAL,PW_RESOURCES,PW_READY_S,PW_OUTPUT,PW_RESIZE,
      PW_LIVE_BEGIN,PW_READY_L,PW_LIVE_OK,PW_CLIENT_LIVE,PW_INPUT,PW_INPUT_OK,
      PW_EXPECT,PW_FINISHED,PW_FINISHED_ACK,PW_SUMMARY,PW_SUMMARY_UPDATE,PW_OUTPUT_SUMMARY,PW_COMMAND,PW_RESULT,PW_PRE_END_INPUT,PW_INPUT_REJECTED,PW_SCHEME,PW_HISTORY};
enum {PW_INPUT_ENDED=1};
typedef struct {uint16_t kind;uint64_t sequence;size_t length;uint8_t bytes[PW_CHUNK];} pw_frame;
/* transfer: positive progress, zero WAIT, negative terminal. Each callback
 * bounded to <=50ms. A frame has one five-second monotonic deadline across
 * header/body; a blob shares ONE deadline across all of its frames. Partial
 * timeout is terminal for this stream; never resume with a new frame/parser. */
typedef struct {void *context;ssize_t (*transfer)(void *,void *,size_t,bool);} pw_io;
void pw_put64(uint8_t *,uint64_t);
uint64_t pw_get64(const uint8_t *);
bool pw_send(pw_io *,uint16_t,uint64_t,const void *,size_t);
bool pw_receive(pw_io *,pw_frame *);
bool pw_send_blob(pw_io *,uint16_t,const void *,size_t);
bool pw_receive_blob(pw_io *,uint16_t,void *,size_t);
ssize_t pw_socket_transfer(void *,void *,size_t,bool);
#endif
