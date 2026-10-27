/* Observational v2.1.0 damage trace. Never call game getters or RNG a second time. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>
#include <math.h>
#include "MinHook.h"
#include "profile.h"

#define CAPACITY 1024
#define STAGES 8
#define RAW_SIZE 152
typedef uintptr_t (*ApplyFn)(void *,void *,uint32_t);
typedef float (*ArmorFn)(void *,float,float,uint8_t,float,uint8_t *);
typedef uint8_t (*RollFn)(void *,float);
typedef float (*BoneFn)(void *,void *,uint8_t *);
typedef float (*HealthFn)(void *,float,uint32_t,float);
static ApplyFn original_apply;
static ArmorFn original_armor;
static RollFn original_roll;
static BoneFn original_bone;
static HealthFn original_health;

typedef struct {
 float damage_in,armor,coefficient,result,ap,human_coef,mutant_coef,min_chance,max_chance;
 uint32_t seed_before,seed_after;
 uint32_t attacker_uid;
 unsigned human,deflected,roll_result,roll_calls,reads,skip_armor,category,source;
} ArmorStage;
typedef struct { float result,multiplier; unsigned bone,reads; } BoneStage;
typedef struct { float requested,current,result; unsigned kind,matching_target; } HealthStage;
typedef struct {
 uint64_t id,parent,begin,end,uptime;
 uintptr_t victim,attack,attributes,modifier;
 uint32_t thread,victim_uid,attacker_uid,flags,read_mask;
 unsigned category,source,armor_count,bone_count,health_count,overflow;
 float hp_before,hp_after,hp_bound;
 double victim_position[3],attacker_position[3];
 unsigned distance_mask;
 uint16_t state_before,state_after,coefficient_half;
 unsigned char raw_attack[RAW_SIZE],raw_attributes[128];
 char label[160];
 ArmorStage armor[STAGES]; BoneStage bone[STAGES]; HealthStage health[STAGES];
} Hit;
typedef struct Context { Hit hit; int active_armor; struct Context *previous; } Context;
static uintptr_t base_address,seed_address;
static uintptr_t parameter_addresses[4];
static uintptr_t entity_pool_address,object_chunks_address,object_count_address,capsule_class_address;
static HMODULE self_module;
static DWORD tls_index=TLS_OUT_OF_INDEXES;
static SRWLOCK queue_lock=SRWLOCK_INIT;
static Hit queue[CAPACITY];
static uint64_t write_index,read_index;
static volatile LONG64 sequence,dropped,emitted;
static volatile LONG capture=1,projectile_only=1,stop_worker,started;
static volatile LONG attacker_filter=-1;
static char test_label[160]="unlabelled";
static wchar_t ini_path[32768],status_path[32768],trace_path[32768];
static FILE *trace_file;
static HANDLE wake_event;

static int read_memory(uintptr_t address,void *out,SIZE_T bytes) {
 SIZE_T got=0;
 return address && ReadProcessMemory(GetCurrentProcess(),(void *)address,out,bytes,&got) && got==bytes;
}
static float read_float(uintptr_t a,unsigned *ok,unsigned bit) {
 float f=0;if(read_memory(a,&f,sizeof(f)))*ok|=bit;return f;
}
/* Read-only equivalents of the audited entity-pool / weak-object lookups.
   A stale UID/serial or an unexpected capsule class invalidates the sample. */
static uintptr_t entity_for_uid(uint32_t uid) {
 if(uid>0x07ffffffu || uid/1024u>255u)return 0;
 uintptr_t chunk=entity_pool_address;
 for(uint32_t page=0;page<uid/1024u;page++)
  if(!read_memory(chunk,&chunk,sizeof(chunk)) || !chunk)return 0;
 uintptr_t entity=chunk+0x10+(uid%1024u)*0x700u;uint32_t actual=0;
 return read_memory(entity+0x10,&actual,4) && actual==uid?entity:0;
}
static uintptr_t actor_for_entity(uintptr_t entity) {
 int32_t weak[2],count=0;uintptr_t chunks=0,chunk=0,actor=0;uint32_t flags=0;int32_t serial=0;
 if(!read_memory(entity+0x50,weak,sizeof(weak)) || weak[0]<0 || weak[1]<=0 ||
    !read_memory(object_count_address,&count,4) || weak[0]>=count ||
    !read_memory(object_chunks_address,&chunks,sizeof(chunks)) || !chunks ||
    !read_memory(chunks+((uint32_t)weak[0]>>16)*sizeof(uintptr_t),&chunk,sizeof(chunk)) || !chunk)return 0;
 uintptr_t slot=chunk+((uint32_t)weak[0]&65535u)*24u;
 if(!read_memory(slot+16,&serial,4) || serial!=weak[1] ||
    !read_memory(slot+8,&flags,4) || (flags&0x10200000u) ||
    !read_memory(slot,&actor,sizeof(actor)))return 0;
 return actor;
}
static int entity_position(uintptr_t entity,double position[3]) {
 uintptr_t actor=actor_for_entity(entity),expected=0;
 if(!actor || !read_memory(capsule_class_address,&expected,sizeof(expected)) || !expected)return 0;
 const unsigned offsets[]={0x340,0x1c0};
 for(unsigned n=0;n<2;n++){
  uintptr_t capsule=0,actual=0;double p[3];
  if(!read_memory(actor+offsets[n],&capsule,sizeof(capsule)) || !capsule ||
     !read_memory(capsule+0x10,&actual,sizeof(actual)) || actual!=expected ||
     !read_memory(capsule+0x1f0,p,sizeof(p)))continue;
  if(!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]))continue;
  memcpy(position,p,sizeof(p));return 1;
 }
 return 0;
}
static Context *current_context(void) { return tls_index==TLS_OUT_OF_INDEXES?NULL:TlsGetValue(tls_index); }
static void status(const char *format,...) {
 FILE *f=_wfopen(status_path,L"ab");if(!f)return;
 fprintf(f,"[%llu] ",(unsigned long long)GetTickCount64());
 va_list args;va_start(args,format);vfprintf(f,format,args);va_end(args);fputc('\n',f);fclose(f);
}
static void enqueue(Hit *h) {
 if(!TryAcquireSRWLockExclusive(&queue_lock)){InterlockedIncrement64(&dropped);return;}
 if(write_index-read_index==CAPACITY)InterlockedIncrement64(&dropped);
 else {queue[write_index%CAPACITY]=*h;write_index++;}
 ReleaseSRWLockExclusive(&queue_lock);
 if(wake_event)SetEvent(wake_event);
}
static int dequeue(Hit *h) {
 int found=0;AcquireSRWLockExclusive(&queue_lock);
 if(read_index<write_index){*h=queue[read_index%CAPACITY];read_index++;found=1;}
 ReleaseSRWLockExclusive(&queue_lock);return found;
}
static void snapshot_before(Hit *h) {
 unsigned mask=0;
 if(read_memory(h->attack,h->raw_attack,sizeof(h->raw_attack))){
  mask|=1;memcpy(&h->attacker_uid,h->raw_attack+0x20,4);
  h->category=h->raw_attack[0x78];h->source=h->raw_attack[0x79];
 }
 if(read_memory(h->victim+0x10,&h->victim_uid,4))mask|=2;
 h->hp_before=read_float(h->victim+0x148,&mask,4);
 h->hp_bound=read_float(h->victim+0x14c,&mask,8);
 if(read_memory(h->victim+0x658,&h->attributes,sizeof(uintptr_t)))mask|=16;
 if(read_memory(h->victim+0x668,&h->modifier,sizeof(uintptr_t)))mask|=32;
 if(read_memory(h->victim+0x260,&h->state_before,2))mask|=64;
 if(h->attributes && read_memory(h->attributes,h->raw_attributes,sizeof(h->raw_attributes)))mask|=128;
 if(h->attributes && read_memory(h->attributes+0x58a,&h->coefficient_half,2))mask|=256;
 if(entity_position(h->victim,h->victim_position))h->distance_mask|=1;
 uintptr_t attacker_entity=(mask&1)?entity_for_uid(h->attacker_uid):0;
 if(attacker_entity && entity_position(attacker_entity,h->attacker_position))h->distance_mask|=2;
 h->read_mask=mask;
}
static uintptr_t hook_apply(void *victim,void *attack,uint32_t flags) {
 DWORD entry_error=GetLastError();unsigned char kind=0;uint32_t attacker=0;
 int take=InterlockedCompareExchange(&capture,0,0)!=0;
 if(take && !read_memory((uintptr_t)attack+0x78,&kind,1))take=0;
 if(take && projectile_only && kind!=6)take=0;
 LONG filter=InterlockedCompareExchange(&attacker_filter,0,0);
 if(take && filter!=-1 && (!read_memory((uintptr_t)attack+0x20,&attacker,4) || attacker!=(uint32_t)filter))take=0;
 if(!take){SetLastError(entry_error);return original_apply(victim,attack,flags);}
 Context ctx;memset(&ctx,0,sizeof(ctx));ctx.active_armor=-1;ctx.previous=current_context();
 Hit *h=&ctx.hit;h->id=(uint64_t)InterlockedIncrement64(&sequence);
 h->parent=ctx.previous?ctx.previous->hit.id:0;h->thread=GetCurrentThreadId();h->flags=flags;
 h->victim=(uintptr_t)victim;h->attack=(uintptr_t)attack;h->uptime=GetTickCount64();
 LARGE_INTEGER q;QueryPerformanceCounter(&q);h->begin=(uint64_t)q.QuadPart;
 if(TryAcquireSRWLockShared(&queue_lock)){memcpy(h->label,test_label,sizeof(h->label));ReleaseSRWLockShared(&queue_lock);}
 else strcpy(h->label,"label_busy");
 snapshot_before(h);
 if(!TlsSetValue(tls_index,&ctx)){InterlockedIncrement64(&dropped);SetLastError(entry_error);return original_apply(victim,attack,flags);}
 SetLastError(entry_error);uintptr_t result=original_apply(victim,attack,flags);DWORD exit_error=GetLastError();
 QueryPerformanceCounter(&q);h->end=(uint64_t)q.QuadPart;
 h->hp_after=read_float(h->victim+0x148,&h->read_mask,512);
 if(read_memory(h->victim+0x260,&h->state_after,2))h->read_mask|=1024;
 TlsSetValue(tls_index,ctx.previous);enqueue(h);SetLastError(exit_error);return result;
}
static float hook_armor(void *attack,float damage,float armor,uint8_t human,float coefficient,uint8_t *deflected) {
 DWORD entry_error=GetLastError();Context *c=current_context();ArmorStage *s=NULL;int previous=-1;
 if(c && c->hit.attack==(uintptr_t)attack){
  unsigned index=c->hit.armor_count++;previous=c->active_armor;c->active_armor=-1;
  if(index<STAGES){s=&c->hit.armor[index];c->active_armor=(int)index;
   s->damage_in=damage;s->armor=armor;s->human=human;s->coefficient=coefficient;
   s->ap=read_float((uintptr_t)attack+8,&s->reads,1);
   if(read_memory((uintptr_t)attack+0x20,&s->attacker_uid,4))s->reads|=512;
   unsigned char kinds[2]={0};if(read_memory((uintptr_t)attack+0x78,kinds,2)){s->category=kinds[0];s->source=kinds[1];s->reads|=1024;}
   unsigned char skip=0;if(read_memory((uintptr_t)attack+0x18,&skip,1)){s->skip_armor=skip;s->reads|=2;}
   s->human_coef=read_float(parameter_addresses[0],&s->reads,4);
   s->mutant_coef=read_float(parameter_addresses[1],&s->reads,8);
   s->min_chance=read_float(parameter_addresses[2],&s->reads,16);
   s->max_chance=read_float(parameter_addresses[3],&s->reads,32);
  }else c->hit.overflow++;
 }
 SetLastError(entry_error);float result=original_armor(attack,damage,armor,human,coefficient,deflected);DWORD exit_error=GetLastError();
 if(s){unsigned char flag=0;s->result=result;if(read_memory((uintptr_t)deflected,&flag,1)){s->deflected=flag;s->reads|=64;}}
 if(c && c->hit.attack==(uintptr_t)attack)c->active_armor=previous;
 SetLastError(exit_error);return result;
}
static uint8_t hook_roll(void *attack,float armor) {
 DWORD entry_error=GetLastError();Context *c=current_context();ArmorStage *s=NULL;
 if(c && c->hit.attack==(uintptr_t)attack && c->active_armor>=0){
  s=&c->hit.armor[c->active_armor];s->roll_calls++;
  if(read_memory(seed_address,&s->seed_before,4))s->reads|=128;
 }
 SetLastError(entry_error);uint8_t result=original_roll(attack,armor);DWORD exit_error=GetLastError();
 if(s){s->roll_result=result;if(read_memory(seed_address,&s->seed_after,4))s->reads|=256;}
 SetLastError(exit_error);return result;
}
static float hook_bone(void *victim,void *attack,uint8_t *bone) {
 DWORD entry_error=GetLastError();SetLastError(entry_error);
 float result=original_bone(victim,attack,bone);DWORD exit_error=GetLastError();Context *c=current_context();
 if(c && c->hit.victim==(uintptr_t)victim && c->hit.attack==(uintptr_t)attack){
  unsigned n=c->hit.bone_count++;
  if(n<STAGES){BoneStage *s=&c->hit.bone[n];s->result=result;unsigned char b=0;
   if(read_memory((uintptr_t)bone,&b,1)){s->bone=b;s->reads|=1;
    if(b && b<32 && c->hit.attributes)s->multiplier=read_float(c->hit.attributes+0x3e0+b*4,&s->reads,2);
   }
  }else c->hit.overflow++;
 }
 SetLastError(exit_error);return result;
}
static float hook_health(void *modifier,float delta,uint32_t kind,float current) {
 DWORD entry_error=GetLastError();SetLastError(entry_error);
 float result=original_health(modifier,delta,kind,current);DWORD exit_error=GetLastError();Context *c=current_context();
 if(c && kind==1){
  unsigned n=c->hit.health_count++;
  if(n<STAGES){HealthStage *s=&c->hit.health[n];s->requested=delta;s->current=current;s->result=result;s->kind=kind;s->matching_target=(uintptr_t)modifier==c->hit.modifier;}
  else c->hit.overflow++;
 }
 SetLastError(exit_error);return result;
}

static void json_string(FILE *f,const char *s) {
 fputc('"',f);for(;*s;s++){
  unsigned char c=(unsigned char)*s;
  if(c=='"' || c=='\\'){fputc('\\',f);fputc(c,f);}
  else if(c<32 || c>=127)fprintf(f,"\\u%04x",c);
  else fputc(c,f);
 }fputc('"',f);
}
static void jf(FILE *f,const char *key,float value) {
 fprintf(f,",\"%s\":",key);if(isfinite(value))fprintf(f,"%.9g",(double)value);else fputs("null",f);
}
static void hex_bytes(FILE *f,const unsigned char *p,size_t n) {fputc('"',f);for(size_t i=0;i<n;i++)fprintf(f,"%02x",p[i]);fputc('"',f);}
static void write_hit(FILE *f,const Hit *h) {
 fprintf(f,"{\"event\":\"hit\",\"id\":%llu,\"parent\":%llu,\"thread\":%u,\"qpc_begin\":%llu,\"qpc_end\":%llu,\"uptime_ms\":%llu,\"label\":",
 (unsigned long long)h->id,(unsigned long long)h->parent,h->thread,(unsigned long long)h->begin,(unsigned long long)h->end,(unsigned long long)h->uptime);json_string(f,h->label);
 fprintf(f,",\"victim_ptr\":\"%llx\",\"attack_ptr\":\"%llx\",\"attributes_ptr\":\"%llx\",\"victim_uid\":%u,\"attacker_uid\":%u,\"category_raw\":%u,\"source_raw\":%u,\"apply_flags\":%u,\"read_mask\":%u,\"state_before\":%u,\"state_after\":%u,\"coefficient_half_bits\":%u",
 (unsigned long long)h->victim,(unsigned long long)h->attack,(unsigned long long)h->attributes,h->victim_uid,h->attacker_uid,h->category,h->source,h->flags,h->read_mask,h->state_before,h->state_after,h->coefficient_half);
 jf(f,"hp_before",h->hp_before);jf(f,"hp_after",h->hp_after);jf(f,"hp_bound_14c",h->hp_bound);
 fprintf(f,",\"distance_read_mask\":%u,\"distance_basis\":\"capsule_centres_at_damage_entry\",\"projectile_travel_cm\":null,\"separation_cm\":",h->distance_mask);
 if((h->distance_mask&3)==3){
  double sum=0;for(unsigned k=0;k<3;k++){double d=h->victim_position[k]-h->attacker_position[k];sum+=d*d;}
  double distance=sqrt(sum);if(isfinite(distance))fprintf(f,"%.17g",distance);else fputs("null",f);
 }else fputs("null",f);
 fputs(",\"attacker_position_cm\":",f);
 if(h->distance_mask&2)fprintf(f,"[%.17g,%.17g,%.17g]",h->attacker_position[0],h->attacker_position[1],h->attacker_position[2]);else fputs("null",f);
 fputs(",\"victim_position_cm\":",f);
 if(h->distance_mask&1)fprintf(f,"[%.17g,%.17g,%.17g]",h->victim_position[0],h->victim_position[1],h->victim_position[2]);else fputs("null",f);
 fprintf(f,",\"armor_count\":%u,\"bone_count\":%u,\"health_count\":%u,\"stage_overflow\":%u,\"armor\":[",h->armor_count,h->bone_count,h->health_count,h->overflow);
 for(unsigned i=0;i<h->armor_count && i<STAGES;i++){
  const ArmorStage *s=&h->armor[i];if(i)fputc(',',f);
  fprintf(f,"{\"reads\":%u,\"human\":%u,\"skip_armor\":%u,\"deflected\":%u,\"roll_result\":%u,\"roll_calls\":%u,\"seed_before\":%u,\"seed_after\":%u,\"attacker_uid\":%u,\"category_raw\":%u,\"source_raw\":%u",s->reads,s->human,s->skip_armor,s->deflected,s->roll_result,s->roll_calls,s->seed_before,s->seed_after,s->attacker_uid,s->category,s->source);
  jf(f,"damage_in",s->damage_in);jf(f,"armor",s->armor);jf(f,"ap",s->ap);jf(f,"coefficient",s->coefficient);jf(f,"result",s->result);
  jf(f,"human_coef",s->human_coef);jf(f,"mutant_coef",s->mutant_coef);jf(f,"min_chance",s->min_chance);jf(f,"max_chance",s->max_chance);fputc('}',f);
 }
 fputs("],\"bones\":[",f);
 for(unsigned i=0;i<h->bone_count && i<STAGES;i++){
  const BoneStage *s=&h->bone[i];if(i)fputc(',',f);fprintf(f,"{\"bone_raw\":%u,\"reads\":%u",s->bone,s->reads);jf(f,"result",s->result);jf(f,"multiplier",s->multiplier);fputc('}',f);
 }
 fputs("],\"health\":[",f);
 for(unsigned i=0;i<h->health_count && i<STAGES;i++){
  const HealthStage *s=&h->health[i];if(i)fputc(',',f);fprintf(f,"{\"kind_raw\":%u,\"matching_target\":%u",s->kind,s->matching_target);jf(f,"requested_delta",s->requested);jf(f,"current",s->current);jf(f,"result",s->result);fputc('}',f);
 }
 fputs("],\"raw_attack_hex\":",f);hex_bytes(f,h->raw_attack,sizeof(h->raw_attack));fputs(",\"raw_attributes_hex\":",f);hex_bytes(f,h->raw_attributes,sizeof(h->raw_attributes));fputs("}\n",f);
}
static int sha256(const void *bytes,ULONG size,unsigned char digest[32]) {
 BCRYPT_ALG_HANDLE alg=NULL;BCRYPT_HASH_HANDLE hash=NULL;int ok=0;
 if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,NULL,0)<0)return 0;
 if(BCryptCreateHash(alg,&hash,NULL,0,NULL,0,0)>=0){ok=BCryptHashData(hash,(PUCHAR)bytes,size,0)>=0 && BCryptFinishHash(hash,digest,32,0)>=0;BCryptDestroyHash(hash);}
 BCryptCloseAlgorithmProvider(alg,0);return ok;
}
static int exe_hash(char text[65]) {
 wchar_t path[32768];if(!GetModuleFileNameW(NULL,path,32768))return 0;
 HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,NULL);
 if(f==INVALID_HANDLE_VALUE)return 0;
 BCRYPT_ALG_HANDLE alg=NULL;BCRYPT_HASH_HANDLE hash=NULL;int ok=0;unsigned char bytes[65536],digest[32];DWORD got;
 if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,NULL,0)>=0 && BCryptCreateHash(alg,&hash,NULL,0,NULL,0,0)>=0){
  for(;;){if(!ReadFile(f,bytes,sizeof(bytes),&got,NULL))break;if(!got){ok=BCryptFinishHash(hash,digest,32,0)>=0;break;}if(BCryptHashData(hash,bytes,got,0)<0)break;}
 }
 if(hash)BCryptDestroyHash(hash);if(alg)BCryptCloseAlgorithmProvider(alg,0);CloseHandle(f);
 if(ok){for(unsigned i=0;i<32;i++)sprintf(text+i*2,"%02x",digest[i]);text[64]=0;}return ok;
}
static int validate_site(uint32_t rva,uint32_t size,const unsigned char expected[32]) {
 unsigned char digest[32];return sha256((void *)(base_address+rva),size,digest) && !memcmp(digest,expected,32);
}
static void settings(void) {
 char label[160]={0};
 /* Wide INI path avoids dependence on the game working directory. */
 wchar_t wide[160];GetPrivateProfileStringW(L"DamageTrace",L"TestLabel",L"unlabelled",wide,160,ini_path);
 WideCharToMultiByte(CP_UTF8,0,wide,-1,label,sizeof(label),NULL,NULL);label[sizeof(label)-1]=0;
 AcquireSRWLockExclusive(&queue_lock);memcpy(test_label,label,sizeof(label));ReleaseSRWLockExclusive(&queue_lock);
 InterlockedExchange(&capture,(LONG)GetPrivateProfileIntW(L"DamageTrace",L"Capture",1,ini_path));
 InterlockedExchange(&projectile_only,(LONG)GetPrivateProfileIntW(L"DamageTrace",L"ProjectileOnly",1,ini_path));
 InterlockedExchange(&attacker_filter,(LONG)GetPrivateProfileIntW(L"DamageTrace",L"AttackerUid",(UINT)-1,ini_path));
}
static int install_hooks(void) {
 const uint32_t rvas[]={RVA_APPLY,RVA_ARMOR,RVA_ROLL,RVA_BONE,RVA_HEALTH};
 void *detours[]={(void *)hook_apply,(void *)hook_armor,(void *)hook_roll,(void *)hook_bone,(void *)hook_health};
 void **originals[]={(void **)&original_apply,(void **)&original_armor,(void **)&original_roll,(void **)&original_bone,(void **)&original_health};
 if(MH_Initialize()!=MH_OK)return 0;
 unsigned created=0;
 for(;created<5;created++)if(MH_CreateHook((void *)(base_address+rvas[created]),detours[created],originals[created])!=MH_OK)break;
 if(created==5){
  unsigned n;for(n=0;n<5;n++)if(MH_QueueEnableHook((void *)(base_address+rvas[n]))!=MH_OK)break;
  if(n==5 && MH_ApplyQueued()==MH_OK)return 1;
 }
 for(unsigned n=0;n<created;n++)MH_QueueDisableHook((void *)(base_address+rvas[n]));
 MH_ApplyQueued();for(unsigned n=0;n<created;n++)MH_RemoveHook((void *)(base_address+rvas[n]));
 return 0;
}
static DWORD WINAPI worker(void *unused) {
 (void)unused;wchar_t directory[32768];DWORD n=GetModuleFileNameW(self_module,directory,32768);
 if(!n || n>32000)return 0;wchar_t *slash=wcsrchr(directory,L'\\');if(!slash)return 0;slash[1]=0;
 swprintf(ini_path,32768,L"%lsDamageTrace.ini",directory);swprintf(status_path,32768,L"%lsDamageTrace.log",directory);
 status("STARTING DamageTrace 0.2.0");
 char actual[65];if(!exe_hash(actual) || strcmp(actual,EXPECTED_EXE_SHA256)){status("UNSUPPORTED: executable SHA256 does not match audited Steam v2.1.0; no hooks installed");return 0;}
 base_address=(uintptr_t)GetModuleHandleW(NULL);seed_address=base_address+0xa291e1c;
 parameter_addresses[0]=base_address+0x9ede3dc;parameter_addresses[1]=base_address+0x9ede3e0;
 parameter_addresses[2]=base_address+0x9ede2e8;parameter_addresses[3]=base_address+0x9ede2e4;
 entity_pool_address=base_address+0xa2f80c0;object_chunks_address=base_address+0xa0d4990;
 object_count_address=base_address+0xa0d49a4;capsule_class_address=base_address+0xa222a58;
 if(!validate_site(RVA_APPLY,SIZE_APPLY,HASH_APPLY) || !validate_site(RVA_ARMOR,SIZE_ARMOR,HASH_ARMOR) || !validate_site(RVA_ROLL,SIZE_ROLL,HASH_ROLL) || !validate_site(RVA_BONE,SIZE_BONE,HASH_BONE) || !validate_site(RVA_HEALTH,SIZE_HEALTH,HASH_HEALTH)){
  status("UNSUPPORTED: live damage code differs from audited functions; no hooks installed");return 0;
 }
 tls_index=TlsAlloc();if(tls_index==TLS_OUT_OF_INDEXES){status("ERROR: no TLS slot");return 0;}
 wchar_t folder[32768];swprintf(folder,32768,L"%lsDamageTrace",directory);
 if(!CreateDirectoryW(folder,NULL) && GetLastError()!=ERROR_ALREADY_EXISTS){status("ERROR: cannot create DamageTrace output directory");return 0;}
 SYSTEMTIME utc;GetSystemTime(&utc);
 swprintf(trace_path,32768,L"%ls\\%04u%02u%02u_%02u%02u%02u_%lu_%llu.jsonl",folder,utc.wYear,utc.wMonth,utc.wDay,utc.wHour,utc.wMinute,utc.wSecond,GetCurrentProcessId(),(unsigned long long)GetTickCount64());
 trace_file=_wfopen(trace_path,L"wb");if(!trace_file){status("ERROR: cannot open JSONL output");return 0;}
 setvbuf(trace_file,NULL,_IOFBF,65536);wake_event=CreateEventW(NULL,FALSE,FALSE,NULL);
 LARGE_INTEGER frequency;QueryPerformanceFrequency(&frequency);
 fprintf(trace_file,"{\"event\":\"session\",\"schema\":2,\"version\":\"0.2.0\",\"exe_sha256\":\"%s\",\"qpc_frequency\":%lld,\"pid\":%lu,\"capacity\":%u}\n",actual,frequency.QuadPart,GetCurrentProcessId(),CAPACITY);fflush(trace_file);
 settings();
 if(!install_hooks()){status("ERROR: hook installation failed");fclose(trace_file);trace_file=NULL;return 0;}
 status("ACTIVE: five damage hooks installed; JSONL writer ready; capture=%ld",capture);
 uint64_t last_flush=0,last_settings=0;
 while(!InterlockedCompareExchange(&stop_worker,0,0)){
  Hit h;unsigned batch=0;
  while(batch++<256 && dequeue(&h)){write_hit(trace_file,&h);InterlockedIncrement64(&emitted);}
  uint64_t now=GetTickCount64();
  if(now-last_settings>=2000){settings();last_settings=now;}
  if(now-last_flush>=1000){
   fprintf(trace_file,"{\"event\":\"heartbeat\",\"uptime_ms\":%llu,\"emitted\":%lld,\"dropped\":%lld,\"capture\":%ld}\n",(unsigned long long)now,InterlockedCompareExchange64(&emitted,0,0),InterlockedCompareExchange64(&dropped,0,0),capture);
   if(fflush(trace_file)!=0 || ferror(trace_file)){InterlockedExchange(&capture,0);status("ERROR: trace write failed; capture stopped");break;}last_flush=now;
  }
  if(wake_event)WaitForSingleObject(wake_event,50);else Sleep(50);
 }
 return 0;
}
__declspec(dllexport) const char *DamageTrace_ThirdPartyLicense(void){return third_party_license;}
__declspec(dllexport) void InitializeASI(void) {
 if(InterlockedCompareExchange(&started,1,0)==0){HANDLE t=CreateThread(NULL,0,worker,NULL,0,NULL);if(t)CloseHandle(t);}
}
#ifndef TRACE_TEST
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID reserved) {
 (void)reserved;
 if(reason==DLL_PROCESS_ATTACH){self_module=module;DisableThreadLibraryCalls(module);InitializeASI();}
 else if(reason==DLL_PROCESS_DETACH)InterlockedExchange(&stop_worker,1);
 return TRUE;
}
#else
#include "selftest.inc"
#endif
