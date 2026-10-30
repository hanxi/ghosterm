#define _POSIX_C_SOURCE 200809L
#include "ghosterm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int failures;
#define CHECK(x,why) do { if (!(x)) { fprintf(stderr,"FAIL connection config: %s\n",why); failures++; } } while(0)
static void issue(const char *text,const char *host,size_t index,const char *key,size_t line,const char *why) {
    gt_config_t *cfg=gt_config_parse(text,strlen(text));
    CHECK(cfg!=NULL,"parse"); if (!cfg) return;
    gt_config_connection_issue_t out={0};
    int rc=gt_config_check_connection(cfg,host,index,&out);
    CHECK(key ? rc==GT_ERR_UNSUPPORTED && out.keyword && !strcmp(out.keyword,key) && out.source_line==line
              : rc==GT_OK && !out.keyword && !out.source_line,why);
    char *after=gt_config_to_string(cfg); CHECK(after && !strcmp(text,after),"inspection preserves bytes");
    free(after); gt_config_free(cfg);
}
int main(int argc,char **argv) {
    CHECK(argc==2,"temporary directory supplied"); if (argc!=2) return 1;
    issue("Host web\n HostName 127.0.0.1\n User deploy\n Port 2222\n IdentityFile ~/.ssh/key\n","web",0,NULL,0,"ordinary profile");
    issue("Host web\n ProxyJump gateway\n","web",0,"proxyjump",2,"jump rejected");
    issue("Host *\n ProxyJump gateway\nHost web\n ProxyJump none\n","web",0,"proxyjump",2,"first winning source");
    issue("Host web\n ProxyJump none\nHost *\n ProxyJump gateway\n","web",0,NULL,0,"shadowed value ignored");
    issue("Host *,!web\n ProxyJump gateway\nHost web\n HostName 127.0.0.1\n","web",0,NULL,0,"negated scope excluded");
    issue("Host other\n ProxyJump gateway\n","web",0,NULL,0,"other host excluded");
    issue("Host web\n LocalForward 80 localhost:80\n LocalForward 81 localhost:81\n","web",1,"localforward",3,"list is cumulative");
    issue("Host web\n SetEnv A=1\n SetEnv B=2\n","web",1,NULL,0,"list first wins");
    issue("Host web\n MysteryDirective private-value\n","web",0,"mysterydirective",2,"unknown directive refused");
    CHECK(strstr(gt_last_error(),"private-value")==NULL,"diagnostic excludes value");
    issue("Host other\n Match exec secret-command\n HostName unreachable\n","web",0,"match",2,"conditional scope refused");
    issue("Host web\n HostName %h.example\n","web",0,"hostname",2,"unimplemented expansion rejected");
    issue("Host web\n IdentityFile ${HOME}/id\n","web",0,"identityfile",2,"environment expansion rejected");
    issue("Host web\n Port 12junk\n","web",0,"port",2,"malformed port rejected");
    issue("Host web\n Port 65536\n","web",0,"port",2,"port upper bound");
    issue("Host web\n Port 0\n","web",0,"port",2,"port lower bound");
    issue("Host web\n Port 65535\n","web",0,NULL,0,"port maximum admitted");
    issue("Host web\n Compression no\n ForwardAgent no\n ServerAliveInterval 0\n StrictHostKeyChecking ask\n AddressFamily any\n","web",0,NULL,0,"explicit inactive settings");
    issue("Host web\n Compression yes\n","web",0,"compression",2,"active compression rejected");
    issue("Host web\n ForwardAgent yes\n","web",0,"forwardagent",2,"agent forwarding rejected");
    issue("Host web\n ServerAliveInterval 60\n","web",0,"serveraliveinterval",2,"keepalive rejected");
    issue("Host web\n User one two\n","web",0,"user",2,"extra basic arguments refused");
    gt_config_connection_issue_t out={0};
    CHECK(gt_config_check_connection(NULL,"web",0,&out)==GT_ERR_INVALID_ARG,"invalid arguments");
    char path[4096],text[8192]; snprintf(path,sizeof path,"%s/connection-config-%ld",argv[1],(long)getpid());
    FILE *f=fopen(path,"wb"); CHECK(f!=NULL,"include fixture"); if (!f) return 1;
    fputs("Host web\n ProxyJump inherited\n",f); fclose(f);
    snprintf(text,sizeof text,"Include %s\n",path);
    gt_config_t *cfg=gt_config_parse(text,strlen(text)); CHECK(cfg!=NULL,"include parse");
    CHECK(gt_config_check_connection(cfg,"web",0,&out)==GT_ERR_UNSUPPORTED && out.included && out.source_line==2 &&
          out.source_path && !strcmp(out.source_path,path),"included source preserved");
    gt_config_free(cfg); unlink(path);
    printf("connection config: %d failures\n",failures); return failures ? 1:0;
}
