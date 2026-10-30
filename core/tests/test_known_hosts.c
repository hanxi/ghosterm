#define _POSIX_C_SOURCE 200809L
/*
 * test_known_hosts.c — known_hosts 读侧的语义（隔离本机真 sshd）。
 *
 * 为什么单拎出来：这里踩过一个**只在特定密钥类型下才暴露**的真缺陷——
 * 过滤 `ss` 前缀是照抄 OpenSSH 对 **ssh-rsa** 的写法，而 ECDSA 的 key type
 * 是 `ecdsa-sha2-nistp256`（根本不以 `ssh` 开头）。于是所有 ECDSA 主机的
 * known_hosts 条目被整条跳过 → 每次连接都判 UNKNOWN → 每次弹信任提示，
 * 落盘再多遍也不管用。
 *
 * 路径：core/src/ssh.c 的 kh_check 是 static，故这里**经真实连接路径**验：
 * 用一份「已经记着本机 sshd 主机密钥」的 known_hosts 连一次，断言
 * **decider 不被调用**（已知且一致 → 直接放行，不弹对话框）。
 */
#include "ghosterm.h"

#include <stdio.h>
#include <arpa/inet.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...)                                        \
  do {                                                       \
    if (c) { g_pass++; }                                     \
    else { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
  } while (0)

static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)n + 1);
  size_t got = fread(buf, 1, (size_t)n, f);
  buf[got] = '\0'; fclose(f);
  return buf;
}

/* 记下 decider 是否被叫过（叫过 = 核心没认出已知主机）。 */
static int g_decider_calls = 0;
static bool g_limit_on_accept = false;
static struct rlimit g_write_limit;
static const char *rewrite_path, *rewrite_text;
static gt_hostkey_decision_t decider(void *ud, const char *host, uint16_t port,
                                     const char *kt, const char *fp) {
  (void)ud; (void)host; (void)port; (void)kt; (void)fp;
  g_decider_calls++;
  if (rewrite_path) {
    FILE *f = fopen(rewrite_path, "wb");
    CHECK(f && fputs(rewrite_text, f) >= 0 && fclose(f) == 0, "consent rewrite fixture");
  }
  if (g_limit_on_accept) CHECK(setrlimit(RLIMIT_FSIZE, &g_write_limit) == 0,
                               "write failure limit installed in decider");
  return GT_HOSTKEY_ACCEPT;
}

/* Real filesystem failures, not mocked success codes. Limit is installed only
 * after trust has been read, and restored before inspecting the result. */
static void persistence_case(const char *label, const char *path,
                             const char *original, const char *host,
                             uint16_t port, const char *user,
                             bool asynchronous, bool limited, bool long_identity) {
  if (original) {
    FILE *f=fopen(path,"wb");
    CHECK(f != NULL,"%s fixture open",label);
    if (!f) return;
    CHECK(fputs(original,f)>=0,"%s fixture write",label);
    CHECK(fclose(f)==0,"%s fixture close",label);
  }
  struct rlimit previous;
  CHECK(getrlimit(RLIMIT_FSIZE,&previous)==0,"%s original limit",label);
  g_write_limit=previous;
  g_write_limit.rlim_cur=original ? strlen(original) : 0;
  void (*old_signal)(int)=signal(SIGXFSZ,SIG_IGN);
  char identity[16384]; memset(identity,'a',sizeof(identity)-1); identity[sizeof(identity)-1]='\0';
  gt_ssh_opts_t opts={.host=long_identity?identity:host,.port=port,.username=user,
                     .known_hosts_path=path,.connect_timeout_ms=3000};
  gt_ssh_t *ssh=gt_ssh_new(&opts);
  CHECK(ssh != NULL,"%s session",label);
  if (!asynchronous) {
    g_decider_calls=0; g_limit_on_accept=limited;
    CHECK(gt_ssh_set_hostkey_policy(ssh,decider,NULL)==GT_OK,"%s policy",label);
    int result=gt_ssh_handshake(ssh);
    g_limit_on_accept=false;
    CHECK(setrlimit(RLIMIT_FSIZE,&previous)==0,"%s restore limit",label);
    CHECK(result==GT_ERR_IO,"%s sync: got %d expected IO",label,result);
    CHECK(g_decider_calls==1,"%s explicit consent exactly once",label);
    CHECK(gt_ssh_auth_methods(ssh)==0,"%s no method discovery after save failure",label);
  } else {
    gt_ssh_auth_options_t options={.family=4,.timeout_ms=3000,.defer_credentials=true};
    CHECK(inet_pton(AF_INET,host,options.address)==1,"%s address",label);
    gt_ssh_authenticator_t *auth=NULL;
    CHECK(gt_ssh_authenticator_open(&ssh,&options,&auth)==GT_OK && !ssh,"%s open",label);
    gt_ssh_auth_phase_t phase=GT_SSH_AUTH_PHASE_TCP;
    for (int i=0;i<4000;i++) {
      phase=gt_ssh_authenticator_step(auth);
      if (phase==GT_SSH_AUTH_PHASE_HOSTKEY || phase==GT_SSH_AUTH_PHASE_FAILED) break;
      struct timespec pause={0,1000000}; nanosleep(&pause,NULL);
    }
    CHECK(phase==GT_SSH_AUTH_PHASE_HOSTKEY,"%s unknown host before save",label);
    if (limited) CHECK(setrlimit(RLIMIT_FSIZE,&g_write_limit)==0,"%s limit",label);
    int result=gt_ssh_authenticator_decide(auth,GT_HOSTKEY_ACCEPT);
    CHECK(setrlimit(RLIMIT_FSIZE,&previous)==0,"%s restore limit",label);
    CHECK(result==GT_ERR_IO,"%s async: got %d expected IO",label,result);
    gt_ssh_auth_info_t info={0};
    CHECK(gt_ssh_authenticator_info(auth,&info),"%s info",label);
    CHECK(info.phase==GT_SSH_AUTH_PHASE_FAILED && info.error==GT_ERR_IO &&
          info.host==GT_SSH_AUTH_HOST_UNKNOWN && info.auth_methods==0,"%s failed before credentials",label);
    CHECK(gt_ssh_authenticator_step(auth)==GT_SSH_AUTH_PHASE_FAILED,"%s failure remains terminal",label);
    gt_ssh_t *taken=NULL;
    CHECK(!gt_ssh_authenticator_take(auth,&taken) && !taken,"%s no session after failed save",label);
    gt_ssh_authenticator_close(auth);
  }
  gt_ssh_close(ssh);
  signal(SIGXFSZ,old_signal);
  if (original) {
    char *after=read_file(path);
    CHECK(after && !strcmp(original,after),"%s existing records unchanged",label);
    free(after);
  } else CHECK(access(path,F_OK)!=0,"%s no file falsely saved",label);
}

/* Both public connection paths must reject before credentials or TOFU. */
static void marker_case(const char *label, const char *contents, const char *path,
                        const char *host, uint16_t port, const char *user,
                        int expected, gt_ssh_auth_host_state_t state) {
  FILE *f = contents ? fopen(path, "wb") : NULL;
  if (contents) {
  CHECK(f != NULL, "%s fixture open", label);
  if (!f) return;
  CHECK(fputs(contents, f) >= 0 && fclose(f) == 0, "%s fixture write", label);
  }
  gt_ssh_opts_t opts = {.host=host, .port=port, .username=user,
                       .known_hosts_path=path, .connect_timeout_ms=3000};
  gt_ssh_t *ssh = gt_ssh_new(&opts);
  g_decider_calls = 0;
  CHECK(ssh && gt_ssh_set_hostkey_policy(ssh, decider, NULL) == GT_OK, "%s policy", label);
  int result = gt_ssh_handshake(ssh);
  CHECK(result == expected, "%s sync: got %d expected %d", label, result, expected);
  CHECK(g_decider_calls == 0, "%s must not ask TOFU", label);
  gt_ssh_close(ssh);
  ssh = gt_ssh_new(&opts);
  gt_ssh_auth_options_t options = {.family=4, .timeout_ms=3000, .defer_credentials=true};
  CHECK(inet_pton(AF_INET, host, options.address) == 1, "%s address", label);
  gt_ssh_authenticator_t *auth = NULL;
  CHECK(gt_ssh_authenticator_open(&ssh, &options, &auth) == GT_OK && !ssh, "%s async open", label);
  gt_ssh_auth_phase_t phase = GT_SSH_AUTH_PHASE_TCP;
  for (int i=0; i<4000; i++) {
    phase = gt_ssh_authenticator_step(auth);
    if (phase == GT_SSH_AUTH_PHASE_FAILED || phase == GT_SSH_AUTH_PHASE_CREDENTIALS ||
        phase == GT_SSH_AUTH_PHASE_HOSTKEY) break;
    struct timespec pause = {0,1000000}; nanosleep(&pause,NULL);
  }
  gt_ssh_auth_info_t info = {0};
  CHECK(gt_ssh_authenticator_info(auth,&info), "%s async info", label);
  CHECK(phase == (expected == GT_OK ? GT_SSH_AUTH_PHASE_CREDENTIALS : GT_SSH_AUTH_PHASE_FAILED),
        "%s async phase %d",label,phase);
  CHECK(info.error == expected && info.host == state, "%s async error %d host %d",label,info.error,info.host);
  if (expected != GT_OK) {
    CHECK(info.auth_methods == 0, "%s must stop before method discovery",label);
    CHECK(gt_ssh_authenticator_decide(auth,GT_HOSTKEY_ACCEPT) == GT_ERR_INVALID_ARG,
          "%s cannot override failure with accept",label);
    gt_ssh_t *taken = NULL;
    CHECK(!gt_ssh_authenticator_take(auth,&taken) && !taken, "%s no trusted session",label);
  }
  gt_ssh_authenticator_close(auth); gt_ssh_close(ssh);
  if (!contents) return;
  char *after=read_file(path);
  CHECK(after && strcmp(after,contents)==0, "%s does not rewrite trust file",label);
  free(after);
}

static void write_trust(const char *path, const char *bytes) {
  FILE *f = fopen(path, "wb");
  CHECK(f && fputs(bytes, f) >= 0 && fclose(f) == 0, "transaction trust fixture");
}
static gt_ssh_authenticator_t *pending_auth(const char *identity, const char *address,
                                         uint16_t port, const char *user, const char *path) {
  gt_ssh_opts_t opts = {.host=identity,.port=port,.username=user,.known_hosts_path=path,.connect_timeout_ms=3000};
  gt_ssh_t *ssh = gt_ssh_new(&opts); gt_ssh_authenticator_t *auth = NULL;
  gt_ssh_auth_options_t options = {.family=4,.timeout_ms=3000,.defer_credentials=true};
  CHECK(inet_pton(AF_INET,address,options.address)==1,"transaction resolved address");
  CHECK(gt_ssh_authenticator_open(&ssh,&options,&auth)==GT_OK && !ssh,"transaction auth owner");
  gt_ssh_auth_phase_t phase = GT_SSH_AUTH_PHASE_TCP;
  for (int i=0;i<3000;i++) {
    phase=gt_ssh_authenticator_step(auth);
    if(phase==GT_SSH_AUTH_PHASE_HOSTKEY || phase==GT_SSH_AUTH_PHASE_FAILED)break;
    struct timespec pause={0,1000000};nanosleep(&pause,NULL);
  }
  CHECK(phase==GT_SSH_AUTH_PHASE_HOSTKEY,"transaction waits for explicit consent");
  return auth;
}
typedef struct { gt_ssh_authenticator_t *auth; gt_status_t status; } acceptance;
static void *accept_thread(void *context) {
  acceptance *a=context; a->status=gt_ssh_authenticator_decide(a->auth,GT_HOSTKEY_ACCEPT);return NULL;
}
static void trust_transactions(const char *path,const char *saved,const char *host,uint16_t port,const char *user) {
  char revoked[8192],mismatch[8192],unsupported[8192];
  snprintf(revoked,sizeof revoked,"@revoked %s",saved);
  snprintf(mismatch,sizeof mismatch,"[%s]:%u ssh-rsa BBBBBBBBBBBBBBBB\n",host,(unsigned)port);
  snprintf(unsupported,sizeof unsupported,"@future %s",saved);
  const char *revisions[]={revoked,mismatch,unsupported,saved};
  int errors[]={GT_ERR_HOST_KEY,GT_ERR_HOST_KEY,GT_ERR_UNSUPPORTED,GT_OK};
  gt_ssh_auth_host_state_t states[]={GT_SSH_AUTH_HOST_REVOKED,GT_SSH_AUTH_HOST_MISMATCH,GT_SSH_AUTH_HOST_UNKNOWN,GT_SSH_AUTH_HOST_UNKNOWN};
  const char *labels[]={"consent-revoked","consent-mismatch","consent-unsupported","consent-already-saved"};
  for(size_t i=0;i<4;i++) {
    write_trust(path,"");
    gt_ssh_authenticator_t *auth=pending_auth(host,host,port,user,path);
    write_trust(path,revisions[i]);
    int rc=gt_ssh_authenticator_decide(auth,GT_HOSTKEY_ACCEPT);
    gt_ssh_auth_info_t info={0}; CHECK(gt_ssh_authenticator_info(auth,&info),"consent info");
    CHECK(rc==errors[i],"%s async result",labels[i]);
    if(errors[i]!=GT_OK)CHECK(info.phase==GT_SSH_AUTH_PHASE_FAILED && info.host==states[i] && info.auth_methods==0,"%s async state before credentials",labels[i]);
    char *after=read_file(path);CHECK(after && !strcmp(after,revisions[i]),"%s no duplicate or override",labels[i]);free(after);
    gt_ssh_authenticator_close(auth);
    write_trust(path,"");
    gt_ssh_opts_t opts={.host=host,.port=port,.username=user,.known_hosts_path=path,.connect_timeout_ms=3000};
    gt_ssh_t *ssh=gt_ssh_new(&opts);gt_ssh_set_hostkey_policy(ssh,decider,NULL);
    rewrite_path=path;rewrite_text=revisions[i];rc=gt_ssh_handshake(ssh);rewrite_path=NULL;
    CHECK(rc==errors[i],"%s sync result",labels[i]);
    if(errors[i]!=GT_OK)CHECK(gt_ssh_auth_methods(ssh)==0,"%s sync no credentials",labels[i]);
    gt_ssh_close(ssh);
  }
  /* A successful append must remain readable by the bounded snapshot API. */
  for (int byte_limit=0;byte_limit<2;byte_limit++) {
    const char *line="other.invalid ssh-rsa AAAAAAAAAAAAAAAA\n";
    size_t length=byte_limit?1024*1024:4096*strlen(line);
    char *full=malloc(length+1); CHECK(full,"trust limit allocation");
    if(!full)return;
    if(byte_limit){memset(full,'a',length);full[0]='#';}
    else for(size_t i=0;i<4096;i++)memcpy(full+i*strlen(line),line,strlen(line));
    full[length]=0;write_trust(path,full);
    gt_ssh_authenticator_t *auth=pending_auth(host,host,port,user,path);
    CHECK(gt_ssh_authenticator_decide(auth,GT_HOSTKEY_ACCEPT)==GT_ERR_IO,
          "consent-%s-limit stops append",byte_limit?"size":"record");
    char *after=read_file(path);CHECK(after && !strcmp(after,full),"consent limit leaves readable original");
    free(after);free(full);gt_ssh_authenticator_close(auth);
  }
  /* Two authenticated sockets independently awaiting consent, same file. */
  for(int distinct=0;distinct<2;distinct++) {
    write_trust(path,"# retained without newline");
    acceptance a={.auth=pending_auth("first.invalid",host,port,user,path)};
    acceptance b={.auth=pending_auth(distinct?"second.invalid":"first.invalid",host,port,user,path)};
    pthread_t ta,tb;
    CHECK(!pthread_create(&ta,NULL,accept_thread,&a) && !pthread_create(&tb,NULL,accept_thread,&b),"concurrent consent threads");
    CHECK(!pthread_join(ta,NULL) && !pthread_join(tb,NULL),"concurrent consent joins");
    CHECK(a.status==GT_OK && b.status==GT_OK,"concurrent consent saved");
    char *after=read_file(path);gt_known_hosts_t *snapshot=after?gt_known_hosts_parse(after,strlen(after)):NULL;
    CHECK(snapshot && gt_known_hosts_count(snapshot)==(size_t)(distinct?2:1),"concurrent consent no loss or duplicate");
    CHECK(after && !strncmp(after,"# retained without newline\n",27),"consent preserves prefix and separates new record");
    gt_known_hosts_free(snapshot);free(after);gt_ssh_authenticator_close(a.auth);gt_ssh_authenticator_close(b.auth);
  }
  /* Removal uses the exact prior revision while a real TOFU writer races it. */
  const char *old="old.invalid ssh-rsa AAAAAAAAAAAAAAAA\n";
  write_trust(path,old);
  acceptance a={.auth=pending_auth(host,host,port,user,path)};pthread_t thread;
  CHECK(!pthread_create(&thread,NULL,accept_thread,&a),"consent/removal thread");
  gt_known_hosts_update_result_t removed;
  gt_status_t status=gt_known_hosts_remove(path,old,strlen(old),0,&removed);
  CHECK(!pthread_join(thread,NULL) && a.status==GT_OK,"consent/removal consent saved");
  CHECK((status==GT_OK && removed.committed) || (status==GT_ERR_IO && removed.conflict && !removed.committed),"consent/removal revision result");
  char *after=read_file(path);
  CHECK(after && strstr(after,saved),"consent/removal never loses new trust");
  CHECK(after && ((strstr(after,old)!=NULL)==!removed.committed),"consent/removal preserves refused revision");
  free(after);gt_ssh_authenticator_close(a.auth);
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc != 6) {
    fprintf(stderr, "usage: %s host port user pem workdir\n", argv[0]);
    return 2;
  }
  const char *host = argv[1];
  int port = atoi(argv[2]);
  const char *user = argv[3];
  char *pem = read_file(argv[4]);
  char kh[1024], kh2[1024];
  snprintf(kh, sizeof kh, "%s/known_hosts", argv[5]);
  snprintf(kh2, sizeof kh2, "%s/known_hosts2", argv[5]);

  /* 1. 先连一次并把主机密钥落盘（与 test_ssh 的 TOFU 用例同法）。 */
  {
    gt_ssh_opts_t o;
    memset(&o, 0, sizeof o);
    o.host = host; o.port = (uint16_t)port; o.username = user;
    o.known_hosts_path = kh;
    gt_ssh_t *s = gt_ssh_new(&o);
    if (s && pem) gt_ssh_auth_private_key(s, pem, NULL);
    if (s) gt_ssh_set_hostkey_policy(s, decider, NULL);
    int rc = gt_ssh_connect(s);
    CHECK(rc == GT_OK, "首次连接（TOFU）成功（rc=%d）", rc);
    gt_ssh_close(s);
  }

  char *saved = read_file(kh);
  CHECK(saved != NULL, "TOFU 已写入 known_hosts");
  if (saved) {
    char transaction_path[1200];snprintf(transaction_path,sizeof transaction_path,"%s/trust-transactions",argv[5]);
    trust_transactions(transaction_path,saved,host,(uint16_t)port,user);
  }
  if (saved) {
    /* 2. **决定性断言**：落盘的 key type 必须能被读侧认出来。
     *    缺陷版本对 ecdsa 条目整条跳过——这里打印出来好定位。 */
    char first[256] = "";
    sscanf(saved, "%255[^\n]", first);
    printf("  known_hosts 首行: %.120s\n", first);
    CHECK(strlen(first) > 0, "known_hosts 有内容");
  }

  /* 3. 用**同一份** known_hosts 再连一次：已知且一致 → decider 不该被调。
   *    缺陷版本每次都会判 UNKNOWN → decider 被调 → UI 弹信任提示。 */
  g_decider_calls = 0;
  {
    gt_ssh_opts_t o;
    memset(&o, 0, sizeof o);
    o.host = host; o.port = (uint16_t)port; o.username = user;
    o.known_hosts_path = kh;
    gt_ssh_t *s = gt_ssh_new(&o);
    if (s && pem) gt_ssh_auth_private_key(s, pem, NULL);
    if (s) gt_ssh_set_hostkey_policy(s, decider, NULL);
    int rc = gt_ssh_connect(s);
    CHECK(rc == GT_OK, "第二次连接成功（rc=%d）", rc);
    gt_ssh_close(s);
  }
  CHECK(g_decider_calls == 0,
        "已知主机重连时**不应**再问用户（decider 被调 %d 次）——"
        "被调说明读侧没认出已知条目（ECDSA 条目的 key type 不以 ssh 开头，"
        "按 `ssh` 前缀过滤会把它们整条跳过）", g_decider_calls);

  /* 3b. **多算法 / 裸名条目**也要能认（变异 K2/K3 的牙就在这里）：
   *     - 服务器常同时提供 ed25519 与 ecdsa 两把主机密钥，known_hosts 里
   *       会有两条不同 key type 的记录——只读第一行就会漏掉第二条；
   *     - 用户手写或 ssh 自己写的裸名条目（`127.0.0.1`，不带端口）
   *       按 OpenSSH 语义匹配**任意端口**。
   *
   *     做法：在落盘的那条**前面**插一条「同主机但不同 key type」的记录，
   *     再把主机名换成裸名形态重连。 */
  if (saved) {
    /* 取出刚才落盘的那条，另存一份用于构造"裸名 + 不同算法"的文件。 */
    char ktype[128] = "", kb64[1024] = "";
    {
      char hosts[256];
      sscanf(saved, "%255s %127s %1023s", hosts, ktype, kb64);
    }
    CHECK(ktype[0] != '\0', "能从 known_hosts 里解析出 key type");
    if (ktype[0]) {
      /* 造一份：先一条**不同**算法的同主机记录（把算法名换掉），
       * 再一条真记录。只读第一行会读到假的那条 → 判 MISMATCH →
       * decider 被调（正是要抓的）。 */
      char fake_type[160];
      snprintf(fake_type, sizeof fake_type, "%s",
               strcmp(ktype, "ssh-ed25519") == 0 ? "ssh-rsa" : "ssh-ed25519");
      char path2[1200];
      snprintf(path2, sizeof path2, "%s/known_hosts_multi", argv[5]);
      FILE *f = fopen(path2, "wb");
      if (f) {
        fprintf(f, "[%s]:%d %s AAAABBBBCCCCDDDDEEEEFFFFGGGGHHHHIIII\n",
                host, port, fake_type);
        fputs(saved, f);
        fclose(f);
      }
      g_decider_calls = 0;
      gt_ssh_opts_t o;
      memset(&o, 0, sizeof o);
      o.host = host; o.port = (uint16_t)port; o.username = user;
      o.known_hosts_path = path2;
      gt_ssh_t *s2 = gt_ssh_new(&o);
      if (s2 && pem) gt_ssh_auth_private_key(s2, pem, NULL);
      if (s2) gt_ssh_set_hostkey_policy(s2, decider, NULL);
      int rc = s2 ? gt_ssh_connect(s2) : GT_OK;
      CHECK(rc == GT_OK, "多算法 known_hosts：真记录在第二条也要认（rc=%d）", rc);
      gt_ssh_close(s2);
      CHECK(g_decider_calls == 0,
            "多算法文件里真记录在第二条：不该问用户（decider %d 次）——"
            "被调说明只读了第一行", g_decider_calls);

      /* 裸名条目（不带端口）必须匹配任意端口。 */
      char path3[1200];
      snprintf(path3, sizeof path3, "%s/known_hosts_bare", argv[5]);
      f = fopen(path3, "wb");
      if (f) {
        fprintf(f, "%s %s %s\n", host, ktype, kb64);
        fclose(f);
      }
      g_decider_calls = 0;
      memset(&o, 0, sizeof o);
      o.host = host; o.port = (uint16_t)port; o.username = user;
      o.known_hosts_path = path3;
      gt_ssh_t *s3 = gt_ssh_new(&o);
      if (s3 && pem) gt_ssh_auth_private_key(s3, pem, NULL);
      if (s3) gt_ssh_set_hostkey_policy(s3, decider, NULL);
      rc = s3 ? gt_ssh_connect(s3) : GT_OK;
      CHECK(rc == GT_OK, "裸名条目应匹配任意端口（rc=%d）", rc);
      gt_ssh_close(s3);
      CHECK(g_decider_calls == 0,
            "裸名（不带端口）条目应认出来（decider %d 次）", g_decider_calls);
    }
  }

  /* Marker precedence, unsupported trust and host-pattern scope. */
  if (saved) {
    char path[1200], text[16384], keytype[128], blob[4096], hosts[512];
    snprintf(path,sizeof path,"%s/known_hosts_markers",argv[5]);
    CHECK(sscanf(saved,"%511s %127s %4095s",hosts,keytype,blob)==3,"marker key fixture");
#define MARKER_CASE(label, expected, state) marker_case(label,text,path,host,(uint16_t)port,user,expected,state)
    snprintf(text,sizeof text,"@revoked %s",saved);
    MARKER_CASE("revoked-only",GT_ERR_HOST_KEY,GT_SSH_AUTH_HOST_REVOKED);
    snprintf(text,sizeof text,"%s@revoked %s",saved,saved);
    MARKER_CASE("revoked-after-trusted",GT_ERR_HOST_KEY,GT_SSH_AUTH_HOST_REVOKED);
    snprintf(text,sizeof text,"@revoked %s%s",saved,saved);
    MARKER_CASE("revoked-before-trusted",GT_ERR_HOST_KEY,GT_SSH_AUTH_HOST_REVOKED);
    snprintf(text,sizeof text,"@cert-authority %s",saved);
    MARKER_CASE("authority-is-not-hostkey",GT_ERR_UNSUPPORTED,GT_SSH_AUTH_HOST_UNCHECKED);
    snprintf(text,sizeof text,"@future-marker %s%s",saved,saved);
    MARKER_CASE("unknown-marker",GT_ERR_UNSUPPORTED,GT_SSH_AUTH_HOST_UNCHECKED);
    snprintf(text,sizeof text,"@revoked * %s %s\n%s",keytype,blob,saved);
    MARKER_CASE("revoked-wildcard",GT_ERR_HOST_KEY,GT_SSH_AUTH_HOST_REVOKED);
    snprintf(text,sizeof text,"@revoked *,!%s %s %s\n%s",host,keytype,blob,saved);
    MARKER_CASE("revoked-negated-host",GT_OK,GT_SSH_AUTH_HOST_KNOWN);
    snprintf(text,sizeof text,"@revoked unrelated.invalid %s %s\n%s",keytype,blob,saved);
    MARKER_CASE("revoked-other-host",GT_OK,GT_SSH_AUTH_HOST_KNOWN);
    snprintf(text,sizeof text,"@revoked %s %s AAAABBBBCCCCDDDDEEEEFFFFGGGGHHHHIIII\n%s",host,keytype,saved);
    MARKER_CASE("revoked-other-key",GT_OK,GT_SSH_AUTH_HOST_KNOWN);
    snprintf(text,sizeof text,"@cert-authority %s%s",saved,saved);
    MARKER_CASE("explicit-plain-trust-with-ca",GT_OK,GT_SSH_AUTH_HOST_KNOWN);
    snprintf(text,sizeof text,"@revoked |1|unsupported|hash %s %s\n%s",keytype,blob,saved);
    MARKER_CASE("hashed-revocation-not-ignored",GT_ERR_UNSUPPORTED,GT_SSH_AUTH_HOST_UNCHECKED);
    marker_case("trust-read-error",NULL,argv[5],host,(uint16_t)port,user,GT_ERR_IO,GT_SSH_AUTH_HOST_UNCHECKED);
#undef MARKER_CASE
  }

  /* 4. 保存失败必须在方法发现/凭据请求前阻断连接。 */
  {
    char path[1200];
    snprintf(path,sizeof path,"%s/missing-parent/known_hosts",argv[5]);
    persistence_case("save-open-failure",path,NULL,host,(uint16_t)port,user,false,false,false);
    persistence_case("save-open-failure",path,NULL,host,(uint16_t)port,user,true,false,false);
    snprintf(path,sizeof path,"%s/known_hosts_write_limit",argv[5]);
    char original[8192];
    const char *key=saved ? strchr(saved,' ') : NULL;
    CHECK(key != NULL,"existing trust record fixture");
    snprintf(original,sizeof original,"unrelated.invalid%s",key ? key : "\n");
    persistence_case("save-flush-failure",path,original,host,(uint16_t)port,user,false,true,false);
    persistence_case("save-flush-failure",path,original,host,(uint16_t)port,user,true,true,false);
    persistence_case("save-write-failure",path,original,host,(uint16_t)port,user,true,true,true);
  }

  /* 5. 密钥变更 → 绝不自动接受（三态不折叠）。 */
  {
    FILE *f = fopen(kh, "wb");
    if (f) { fputs("garbage-host ssh-ed25519 AAAAgarbage\n", f); fclose(f); }
    gt_ssh_opts_t o;
    memset(&o, 0, sizeof o);
    o.host = host; o.port = (uint16_t)port; o.username = user;
    o.known_hosts_path = kh2;
    gt_ssh_t *s = gt_ssh_new(&o);
    if (s && pem) gt_ssh_auth_private_key(s, pem, NULL);
    int rc = s ? gt_ssh_connect(s) : GT_OK;
    CHECK(rc == GT_ERR_HOST_KEY,
          "空 known_hosts 且无 decider → GT_ERR_HOST_KEY（不静默 TOFU）");
    gt_ssh_close(s);
  }

  free(saved);
  free(pem);
  printf("test_known_hosts: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
