/* Private P0 config transaction; each value owns an independent GUI-pool lease. */
#ifndef GT_P0_CONFIG_OWNER_H
#define GT_P0_CONFIG_OWNER_H
#include <ghostty.h>
#include "gui-budget.h"
typedef struct {p0_gui_reservation *reservation;ghostty_config_t value;} p0_config_owner;
static inline bool p0_config_close(p0_config_owner *o){
    if(!o)return false;
    if(o->value){ghostty_config_free(o->value);o->value=NULL;}
    if(o->reservation){if(p0_gui_unreserve(o->reservation)!=P0_GUI_OK)return false;o->reservation=NULL;}
    return true;
}
static inline bool p0_config_open(p0_gui_pool *pool,size_t capacity,p0_config_owner *o){
    if(!o || o->value || o->reservation)return false;
    if(p0_gui_reserve(pool,capacity,&o->reservation)!=P0_GUI_OK)return false;
    o->value=ghostty_p0_config_new(p0_gui_reserved_allocator(o->reservation));
    if(!o->value || !p0_gui_reserved_healthy(o->reservation)){(void)p0_config_close(o);return false;}
    return true;
}
static inline bool p0_config_load(p0_config_owner *o,const char *path){
    if(!o || !o->value)return false;
    if(!ghostty_p0_config_load_file(o->value,path) || !p0_gui_reserved_healthy(o->reservation)){(void)p0_config_close(o);return false;}
    return true;
}
static inline bool p0_config_finish(p0_config_owner *o){
    if(!o || !o->value)return false;
    if(!ghostty_p0_config_finalize(o->value) || !p0_gui_reserved_healthy(o->reservation)){(void)p0_config_close(o);return false;}
    return true;
}
#endif
