#include "android_graphics.h"
#include "android_png.h"
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PLACEMENTS (256u*256u+4096u)
#define MAX_IMAGES 1024
#define RECORD 10
static atomic_llong bitmap_count,frame_count;
#define BITMAP_BUDGET (24u*1024u*1024u)
typedef struct image_cache {
    struct image_cache *next;
    uint64_t generation;
    uint32_t id,width,height;
    jobject bitmap;
    bool seen;
} image_cache;
typedef struct { uint32_t image,placement,ox,oy;int32_t z;GhosttyKittyGraphicsPlacementRenderInfo rect; } placement;
struct gat_graphics_frame {
    atomic_uint refs;
    size_t image_count,count;
    int32_t *records;
    uint64_t *generations;
    jobject *bitmaps;
};
struct gat_graphics_cache { image_cache *images;gat_graphics_frame *last; };
gat_graphics_cache *gat_graphics_new(void) { return calloc(1,sizeof(gat_graphics_cache)); }
void gat_graphics_frame_retain(gat_graphics_frame *frame) { if(frame)atomic_fetch_add(&frame->refs,1); }
void gat_graphics_frame_free(gat_graphics_frame *frame) {
    if(!frame || atomic_fetch_sub(&frame->refs,1)!=1)return;
    atomic_fetch_sub(&frame_count,1);
    bool attached=false;JNIEnv *env=gat_android_env_acquire(&attached);
    if(env && frame->bitmaps)for(size_t i=0;i<frame->image_count;i++)if(frame->bitmaps[i])(*env)->DeleteGlobalRef(env,frame->bitmaps[i]);
    gat_android_env_release(attached);
    free(frame->records);free(frame->generations);free(frame->bitmaps);free(frame);
}
void gat_graphics_free(gat_graphics_cache *cache) {
    if(!cache)return;
    bool attached=false;JNIEnv *env=gat_android_env_acquire(&attached);
    image_cache *image=cache->images;
    while(image) { image_cache *next=image->next;if(env)(*env)->DeleteGlobalRef(env,image->bitmap);atomic_fetch_sub(&bitmap_count,1);free(image);image=next; }
    gat_android_env_release(attached);gat_graphics_frame_free(cache->last);free(cache);
}
static int ordered(const void *a,const void *b) {
    const placement *x=a,*y=b;
    if(x->z!=y->z)return x->z<y->z?-1:1;
    if(x->image!=y->image)return x->image<y->image?-1:1;
    return x->placement<y->placement?-1:x->placement!=y->placement;
}
static image_cache *bitmap(gat_graphics_cache *cache,JNIEnv *env,GhosttyKittyGraphics storage,uint32_t id) {
    GhosttyKittyGraphicsImage image=ghostty_kitty_graphics_image(storage,id);
    if(!image)return NULL;
    uint64_t generation=0;uint32_t w=0,h=0;size_t length=0;const uint8_t *pixels=NULL;GhosttyKittyImageFormat format;
#define GET(key,out) if(ghostty_kitty_graphics_image_get(image,key,out)!=GHOSTTY_SUCCESS)return NULL
    GET(GHOSTTY_KITTY_IMAGE_DATA_GENERATION,&generation);
    for(image_cache *found=cache->images;found;found=found->next)if(found->id==id && found->generation==generation) { found->seen=true;return found; }
    GET(GHOSTTY_KITTY_IMAGE_DATA_WIDTH,&w);GET(GHOSTTY_KITTY_IMAGE_DATA_HEIGHT,&h);
    GET(GHOSTTY_KITTY_IMAGE_DATA_FORMAT,&format);GET(GHOSTTY_KITTY_IMAGE_DATA_DATA_PTR,&pixels);GET(GHOSTTY_KITTY_IMAGE_DATA_DATA_LEN,&length);
#undef GET
    const size_t channels=format==GHOSTTY_KITTY_IMAGE_FORMAT_RGB?3:4;
    if(!w || !h || w>4096 || h>4096 || (format!=GHOSTTY_KITTY_IMAGE_FORMAT_RGB && format!=GHOSTTY_KITTY_IMAGE_FORMAT_RGBA) || length!=(size_t)w*h*channels)return NULL;
    jobject copy=gat_android_bitmap_copy(env,w,h,pixels,(int)format);
    if(!copy)return NULL;
    image_cache *result=calloc(1,sizeof(*result));
    if(!result) { (*env)->DeleteGlobalRef(env,copy);return NULL; }
    *result=(image_cache){.next=cache->images,.id=id,.generation=generation,.width=w,.height=h,.bitmap=copy,.seen=true};
    cache->images=result;atomic_fetch_add(&bitmap_count,1);return result;
}
typedef struct { placement *items;size_t count,capacity; } placement_list;
static bool append(placement_list *out,placement p) {
    if(out->count==MAX_PLACEMENTS)return false;
    if(out->count==out->capacity) {
        size_t capacity=out->capacity?out->capacity*2:16;
        if(capacity>MAX_PLACEMENTS)capacity=MAX_PLACEMENTS;
        placement *items=realloc(out->items,capacity*sizeof(*items));
        if(!items)return false;
        out->items=items;out->capacity=capacity;
    }
    out->items[out->count++]=p;return true;
}
static bool virtual_placement(void *context,uint32_t image,uint32_t id,int32_t z,uint32_t x,uint32_t y,const GhosttyKittyGraphicsPlacementRenderInfo *rect) {
    return append(context,(placement){.image=image,.placement=id,.z=z,.ox=x,.oy=y,.rect=*rect});
}
bool gat_graphics_capture(gat_graphics_cache *cache,GhosttyTerminal terminal,uint16_t cw,uint16_t ch,gat_graphics_frame **out,bool *changed) {
    *out=NULL;*changed=false;if(!cache || !terminal || !cw || !ch)return false;
    bool attached=false,ok=false;JNIEnv *env=gat_android_env_acquire(&attached);if(!env)return false;
    GhosttyKittyGraphics storage=NULL;GhosttyKittyGraphicsPlacementIterator iterator=NULL;
    placement_list list={0};
    placement *placements=NULL;int32_t *records=NULL;
    image_cache **images=calloc(MAX_IMAGES,sizeof(*images));
    size_t count=0,image_count=0,bytes=0;gat_graphics_frame *frame=NULL;
    if(!images)goto done;
    if(ghostty_terminal_get(terminal,GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS,&storage)!=GHOSTTY_SUCCESS ||
       ghostty_kitty_graphics_placement_iterator_new(NULL,&iterator)!=GHOSTTY_SUCCESS ||
       ghostty_kitty_graphics_get(storage,GHOSTTY_KITTY_GRAPHICS_DATA_PLACEMENT_ITERATOR,&iterator)!=GHOSTTY_SUCCESS)goto done;
    while(ghostty_kitty_graphics_placement_next(iterator)) {
        placement p={.rect=GHOSTTY_INIT_SIZED(GhosttyKittyGraphicsPlacementRenderInfo)};
#define PLACEMENT(key,out) if(ghostty_kitty_graphics_placement_get(iterator,key,out)!=GHOSTTY_SUCCESS)goto done
        PLACEMENT(GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_IMAGE_ID,&p.image);
        PLACEMENT(GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_PLACEMENT_ID,&p.placement);
        PLACEMENT(GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_Z,&p.z);
        PLACEMENT(GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_X_OFFSET,&p.ox);
        PLACEMENT(GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_Y_OFFSET,&p.oy);
#undef PLACEMENT
        GhosttyKittyGraphicsImage image=ghostty_kitty_graphics_image(storage,p.image);
        if(!image || ghostty_kitty_graphics_placement_render_info(iterator,image,terminal,&p.rect)!=GHOSTTY_SUCCESS)goto done;
        if(!p.rect.viewport_visible)continue;
        if(!append(&list,p))goto done;
    }
    if(ghostty_kitty_graphics_virtual_render(terminal,cw,ch,virtual_placement,&list)!=GHOSTTY_SUCCESS)goto done;
    placements=list.items;count=list.count;
    records=calloc(count?count*RECORD:1,sizeof(*records));if(!records)goto done;
    if(count>1)qsort(placements,count,sizeof(*placements),ordered);
    for(image_cache *old=cache->images;old;old=old->next)old->seen=false;
    for(size_t i=0;i<count;i++) {
        placement *p=&placements[i];image_cache *image=bitmap(cache,env,storage,p->image);if(!image)goto done;
        size_t index=0;while(index<image_count && images[index]!=image)index++;
        if(index==image_count) {
            if(image_count==MAX_IMAGES || (uint64_t)image->width*image->height*4>BITMAP_BUDGET-bytes)goto done;
            bytes+=(size_t)image->width*image->height*4;images[image_count++]=image;
        }
        const GhosttyKittyGraphicsPlacementRenderInfo *r=&p->rect;
        int64_t x=(int64_t)r->viewport_col*cw+p->ox,y=(int64_t)r->viewport_row*ch+p->oy;
        if(x<INT32_MIN || y<INT32_MIN || x+r->pixel_width>INT32_MAX || y+r->pixel_height>INT32_MAX)goto done;
        int32_t *record=records+i*RECORD;
        record[0]=(int32_t)index;record[1]=(int32_t)r->source_x;record[2]=(int32_t)r->source_y;
        record[3]=(int32_t)r->source_width;record[4]=(int32_t)r->source_height;
        record[5]=(int32_t)x;record[6]=(int32_t)y;record[7]=(int32_t)(x+r->pixel_width);record[8]=(int32_t)(y+r->pixel_height);record[9]=p->z;
    }
    bool same=cache->last && cache->last->count==count && cache->last->image_count==image_count &&
        (!count || !memcmp(cache->last->records,records,count*RECORD*sizeof(*records)));
    for(size_t i=0;same && i<image_count;i++)same=cache->last->generations[i]==images[i]->generation;
    if(!same) {
        frame=calloc(1,sizeof(*frame));if(!frame)goto done;
        atomic_init(&frame->refs,1);atomic_fetch_add(&frame_count,1);frame->count=count;frame->image_count=image_count;
        frame->records=records;records=NULL;
        frame->generations=calloc(image_count?image_count:1,sizeof(*frame->generations));
        frame->bitmaps=calloc(image_count?image_count:1,sizeof(*frame->bitmaps));
        if(!frame->generations || !frame->bitmaps)goto done;
        for(size_t i=0;i<image_count;i++) {
            frame->generations[i]=images[i]->generation;
            frame->bitmaps[i]=(*env)->NewGlobalRef(env,images[i]->bitmap);
            if(!frame->bitmaps[i])goto done;
        }
        gat_graphics_frame_free(cache->last);cache->last=frame;frame=NULL;
        *out=cache->last;gat_graphics_frame_retain(*out);*changed=true;
    }
    image_cache **old=&cache->images;
    while(*old) {
        if((*old)->seen) { old=&(*old)->next;continue; }
        image_cache *removed=*old;*old=removed->next;(*env)->DeleteGlobalRef(env,removed->bitmap);atomic_fetch_sub(&bitmap_count,1);free(removed);
    }
    ok=true;
done:
    ghostty_kitty_graphics_placement_iterator_free(iterator);gat_graphics_frame_free(frame);
    free(list.items);free(records);free(images);
    if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);
    gat_android_env_release(attached);return ok;
}
jobjectArray gat_graphics_frame_java(JNIEnv *env,gat_graphics_frame *frame,uint64_t revision) {
    if(!frame)return NULL;
    jclass object=(*env)->FindClass(env,"java/lang/Object"),bitmap=(*env)->FindClass(env,"android/graphics/Bitmap");
    if(!object || !bitmap)return NULL;
    jobjectArray packet=(*env)->NewObjectArray(env,2,object,NULL);
    jintArray records=(*env)->NewIntArray(env,(jsize)(4+frame->count*RECORD));
    jobjectArray images=(*env)->NewObjectArray(env,(jsize)frame->image_count,bitmap,NULL);
    if(packet && records && images) {
        jint header[]={(jint)revision,(jint)(revision>>32),1,(jint)frame->count};
        (*env)->SetIntArrayRegion(env,records,0,4,header);
        if(frame->count)(*env)->SetIntArrayRegion(env,records,4,(jsize)(frame->count*RECORD),frame->records);
        for(size_t i=0;i<frame->image_count;i++)(*env)->SetObjectArrayElement(env,images,(jsize)i,frame->bitmaps[i]);
        (*env)->SetObjectArrayElement(env,packet,0,records);(*env)->SetObjectArrayElement(env,packet,1,images);
    } else packet=NULL;
    if(records)(*env)->DeleteLocalRef(env,records);if(images)(*env)->DeleteLocalRef(env,images);
    (*env)->DeleteLocalRef(env,object);(*env)->DeleteLocalRef(env,bitmap);return packet;
}

void gat_graphics_metrics(int64_t *bitmaps,int64_t *frames) { *bitmaps=atomic_load(&bitmap_count);*frames=atomic_load(&frame_count); }
