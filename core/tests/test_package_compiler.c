/* Default upstream allocator, separate from the native checker allocator.
 * Optimized ASan builds exercise the actual shipped trusted metadata corpus. */
#include <quickjs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "extension/package-validator-code.inc"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL package-compiler:%d %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
 size_t at=0;char *code=malloc(GT_PACKAGE_VALIDATOR_BYTES+1);CHECK(code);
 for(size_t i=0;i<sizeof(package_validator_chunks)/sizeof(package_validator_chunks[0]);i++){size_t n=strlen(package_validator_chunks[i]);CHECK(n<=GT_PACKAGE_VALIDATOR_BYTES-at);memcpy(code+at,package_validator_chunks[i],n);at+=n;}CHECK(at==GT_PACKAGE_VALIDATOR_BYTES);code[at]=0;
 JSRuntime *rt=JS_NewRuntime();CHECK(rt);JS_SetMemoryLimit(rt,67108864);JS_SetCanBlock(rt,false);JS_SetMaxStackSize(rt,524288);JSContext *c=JS_NewContext(rt);CHECK(c);
 JSValue api=JS_Eval(c,code,at,"default-allocator-package-corpus",JS_EVAL_TYPE_GLOBAL|JS_EVAL_FLAG_STRICT);CHECK(!JS_IsException(api));JSValue fn=JS_GetPropertyStr(c,api,"check");CHECK(JS_IsFunction(c,fn));JS_FreeValue(c,fn);JS_FreeValue(c,api);JS_FreeContext(c);JS_FreeRuntime(rt);free(code);printf("PACKAGE COMPILER PASS: %u bytes / default allocator\n",GT_PACKAGE_VALIDATOR_BYTES);return 0;
}
