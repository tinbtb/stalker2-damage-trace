/* Native detour/ABI and logger tests. No game process is started or modified. */
static uint32_t test_seed=1;
static float test_parameters[4]={.4f,.4f,.1f,.9f};
static ApplyFn volatile call_apply;
static ArmorFn volatile call_armor;
static RollFn volatile call_roll;
static BoneFn volatile call_bone;
static HealthFn volatile call_health;
static unsigned mock_counts[5];
static int nested;
static uint8_t test_bone=1;
static float post_armor_scale=1;
static unsigned char other_victim[0x700],other_attack[RAW_SIZE];
static unsigned char fake_pool[0x10+1024*0x700],fake_slots[3*24];
static unsigned char fake_actors[3][0x400],fake_capsules[3][0x520];
static uintptr_t fake_chunks[1],fake_chunks_pointer,fake_capsule_class=0x12345678;
static int32_t fake_object_count=3;
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
__declspec(noinline) static uint8_t mock_roll(void *attack,float armor) {
 mock_counts[2]++;float ap;memcpy(&ap,(char *)attack+8,4);
 float threshold=fmaxf(fminf(ap-armor+1,test_parameters[3]),test_parameters[2]);
 test_seed=test_seed*196314165u+907633515u;
 float roll=(float)(test_seed>>9)/8388608.0f;return (uint8_t)(roll>threshold);
}
__declspec(noinline) static float mock_armor(void *attack,float damage,float armor,uint8_t human,float coefficient,uint8_t *deflected) {
 mock_counts[1]++;float ap;memcpy(&ap,(char *)attack+8,4);
 *deflected=call_roll(attack,armor);
 if(*deflected)damage*=test_parameters[human?0:1];
 return damage*powf(coefficient,ap-armor);
}
__declspec(noinline) static float mock_bone(void *victim,void *attack,uint8_t *bone) {
 mock_counts[3]++;*bone=test_bone;float d,coefficient;uintptr_t attributes;
 memcpy(&d,attack,4);memcpy(&attributes,(char *)victim+0x658,sizeof(attributes));
 memcpy(&coefficient,(void *)(attributes+0x3e0+test_bone*4),4);return d*coefficient;
}
__declspec(noinline) static float mock_health(void *modifier,float delta,uint32_t kind,float current) {
 (void)modifier;mock_counts[4]++;return kind==1?fmaxf(0,fminf(1000,current+delta)):current;
}
__declspec(noinline) static uintptr_t mock_apply(void *victim,void *attack,uint32_t flags) {
 mock_counts[0]++;uint8_t bone=0,deflected=0;
 if(nested && victim!=other_victim)call_apply(other_victim,other_attack,flags);
 float before=call_bone(victim,attack,&bone);
 float final=call_armor(attack,before,2,1,1.5f,&deflected);
 final*=post_armor_scale;
 float hp;uintptr_t modifier;memcpy(&hp,(char *)victim+0x148,4);memcpy(&modifier,(char *)victim+0x668,sizeof(modifier));
 hp=call_health((void *)modifier,-final,1,hp);memcpy((char *)victim+0x148,&hp,4);
 SetLastError(4321);return (uintptr_t)0x12345678u;
}
static void fixture(unsigned char *victim,unsigned char *attack,unsigned char *attributes,uint32_t uid) {
 memset(victim,0,0x700);memset(attack,0,RAW_SIZE);memset(attributes,0,0x600);
 float hp=200,d=25,ap=1,bone=4;uintptr_t attr=(uintptr_t)attributes,modifier=(uintptr_t)victim+0x680;
 memcpy(victim+0x148,&hp,4);memcpy(victim+0x14c,&hp,4);memcpy(victim+0x10,&uid,4);
 memcpy(victim+0x658,&attr,sizeof(attr));memcpy(victim+0x668,&modifier,sizeof(modifier));
 memcpy(attack,&d,4);memcpy(attack+8,&ap,4);uid=7;memcpy(attack+0x20,&uid,4);attack[0x78]=6;
 memcpy(attributes+0x3e4,&bone,4);
 bone=12;memcpy(attributes+0x3e0,&bone,4);
}
static void distance_fixture(unsigned char *victim) {
 memset(fake_pool,0,sizeof(fake_pool));memset(fake_slots,0,sizeof(fake_slots));
 entity_pool_address=(uintptr_t)fake_pool;object_chunks_address=(uintptr_t)&fake_chunks_pointer;
 object_count_address=(uintptr_t)&fake_object_count;capsule_class_address=(uintptr_t)&fake_capsule_class;
 fake_chunks[0]=(uintptr_t)fake_slots;fake_chunks_pointer=(uintptr_t)fake_chunks;
 for(unsigned i=0;i<3;i++){
  uintptr_t actor=(uintptr_t)fake_actors[i],capsule=(uintptr_t)fake_capsules[i];
  int32_t serial=100+(int32_t)i;
  memcpy(fake_slots+i*24,&actor,sizeof(actor));memcpy(fake_slots+i*24+16,&serial,4);
  memcpy(fake_actors[i]+0x340,&capsule,sizeof(capsule));memcpy(fake_capsules[i]+0x10,&fake_capsule_class,sizeof(fake_capsule_class));
 }
 unsigned char *shooter=fake_pool+0x10+7*0x700;uint32_t uid=7;memcpy(shooter+0x10,&uid,4);
 int32_t source_weak[2]={0,100},victim_weak[2]={1,101},other_weak[2]={2,102};
 memcpy(shooter+0x50,source_weak,8);memcpy(victim+0x50,victim_weak,8);memcpy(other_victim+0x50,other_weak,8);
 double source[3]={0,0,0},target[3]={300,400,0};
 memcpy(fake_capsules[0]+0x1f0,source,24);memcpy(fake_capsules[1]+0x1f0,target,24);memcpy(fake_capsules[2]+0x1f0,target,24);
}
int main(void) {
 unsigned char victim[0x700],attack[RAW_SIZE],attributes[0x600],other_attributes[0x600];
 fixture(victim,attack,attributes,42);fixture(other_victim,other_attack,other_attributes,43);
 distance_fixture(victim);
 tls_index=TlsAlloc();CHECK(tls_index!=TLS_OUT_OF_INDEXES);seed_address=(uintptr_t)&test_seed;
 for(unsigned n=0;n<4;n++)parameter_addresses[n]=(uintptr_t)&test_parameters[n];
 CHECK(MH_Initialize()==MH_OK);
 void *targets[]={(void *)mock_apply,(void *)mock_armor,(void *)mock_roll,(void *)mock_bone,(void *)mock_health};
 void *detours[]={(void *)hook_apply,(void *)hook_armor,(void *)hook_roll,(void *)hook_bone,(void *)hook_health};
 void **originals[]={(void **)&original_apply,(void **)&original_armor,(void **)&original_roll,(void **)&original_bone,(void **)&original_health};
 for(unsigned n=0;n<5;n++)CHECK(MH_CreateHook(targets[n],detours[n],originals[n])==MH_OK);
 call_apply=mock_apply;call_armor=mock_armor;call_roll=mock_roll;call_bone=mock_bone;call_health=mock_health;
 CHECK(MH_EnableHook(MH_ALL_HOOKS)==MH_OK);
 SetLastError(1234);CHECK(call_apply(victim,attack,99)==0x12345678u);CHECK(GetLastError()==4321);
 Hit hit;CHECK(dequeue(&hit));CHECK(hit.victim_uid==42 && hit.attacker_uid==7 && hit.flags==99);
 CHECK(hit.read_mask==2047);CHECK(hit.armor_count==1 && hit.bone_count==1 && hit.health_count==1);
 CHECK(hit.armor[0].damage_in==100 && hit.armor[0].armor==2 && hit.armor[0].coefficient==1.5f);
 CHECK(hit.armor[0].reads==2047 && hit.armor[0].seed_after==test_seed);
 CHECK(hit.bone[0].bone==1 && hit.bone[0].multiplier==4);
 CHECK(hit.health[0].matching_target==1 && hit.health[0].requested==-hit.armor[0].result);
 CHECK(hit.hp_after==hit.health[0].result && hit.hp_before==200);
 CHECK(hit.distance_mask==3 && hit.victim_position[0]==300 && hit.victim_position[1]==400);
 CHECK(hit.attacker_position[0]==0 && hit.attacker_position[1]==0);
 /* Stale weak handles, reused entity IDs and unexpected classes must fail
    rather than fabricate a zero-distance measurement. */
 double position[3];int32_t bad_serial=999;
 memcpy(victim+0x54,&bad_serial,4);CHECK(!entity_position((uintptr_t)victim,position));
 int32_t good_serial=101;memcpy(victim+0x54,&good_serial,4);
 CHECK(entity_position((uintptr_t)victim,position));
 uint32_t wrong_uid=8;memcpy(fake_pool+0x10+7*0x700+0x10,&wrong_uid,4);CHECK(!entity_for_uid(7));
 wrong_uid=7;memcpy(fake_pool+0x10+7*0x700+0x10,&wrong_uid,4);
 uintptr_t wrong_class=0;memcpy(fake_capsules[1]+0x10,&wrong_class,sizeof(wrong_class));CHECK(!entity_position((uintptr_t)victim,position));
 memcpy(fake_capsules[1]+0x10,&fake_capsule_class,sizeof(fake_capsule_class));
 for(unsigned n=0;n<5;n++)CHECK(mock_counts[n]==1);
 FILE *f=fopen("selftest.jsonl","wb");CHECK(f);write_hit(f,&hit);
 /* Head is enum zero: capture its coefficient and successful read mask. */
 test_bone=0;call_apply(victim,attack,0);test_bone=1;
 Hit head;CHECK(dequeue(&head));
 CHECK(head.bone[0].bone==0 && head.bone[0].multiplier==12 && head.bone[0].reads==3);
 CHECK(head.bone[0].result==300 && head.armor[0].damage_in==300);
 fixture(victim,attack,attributes,42);distance_fixture(victim);
 /* Nested calls retain separate events and restore their parent's context. */
 nested=1;CHECK(call_apply(victim,attack,0)==0x12345678u);nested=0;
 Hit child,parent;CHECK(dequeue(&child));CHECK(dequeue(&parent));
 CHECK(child.parent==parent.id && parent.parent==0 && current_context()==NULL);
 CHECK(child.victim_uid==43 && parent.victim_uid==42);
 write_hit(f,&child);write_hit(f,&parent);
 /* A post-armor adjustment must remain observable rather than be mistaken for
    an armor formula error. A lethal hit must retain the unclamped request. */
 fixture(victim,attack,attributes,42);post_armor_scale=.5f;call_apply(victim,attack,0);post_armor_scale=1;
 CHECK(dequeue(&hit));CHECK(hit.health[0].requested==-hit.armor[0].result*.5f);write_hit(f,&hit);
 fixture(victim,attack,attributes,42);float lethal=200;memcpy(attack,&lethal,4);call_apply(victim,attack,0);
 CHECK(dequeue(&hit));CHECK(hit.hp_after==0 && -hit.health[0].requested>hit.hp_before);write_hit(f,&hit);
 /* Filtered and disabled paths still invoke the original exactly once. */
 LONG64 before=sequence;capture=0;unsigned previous=mock_counts[0];call_apply(victim,attack,0);
 CHECK(sequence==before && mock_counts[0]==previous+1 && !dequeue(&hit));capture=1;
 attacker_filter=8;call_apply(victim,attack,0);CHECK(sequence==before && !dequeue(&hit));attacker_filter=-1;
 /* Queue saturation and contention are explicitly counted, never overwritten. */
 memset(&hit,0,sizeof(hit));for(unsigned n=0;n<CAPACITY;n++){hit.id=n;enqueue(&hit);}
 LONG64 dropped_before=dropped;enqueue(&hit);CHECK(dropped==dropped_before+1);
 CHECK(dequeue(&hit) && hit.id==0);while(dequeue(&hit)){}
 AcquireSRWLockExclusive(&queue_lock);enqueue(&hit);ReleaseSRWLockExclusive(&queue_lock);CHECK(dropped==dropped_before+2);
 /* NaNs must become valid JSON null; labels must be escaped. */
 memset(&hit,0,sizeof(hit));strcpy(hit.label,"quote\" slash\\ newline\n");hit.hp_before=NAN;write_hit(f,&hit);CHECK(fclose(f)==0);
 CHECK(MH_DisableHook(MH_ALL_HOOKS)==MH_OK);CHECK(MH_Uninitialize()==MH_OK);TlsFree(tls_index);
 puts("PASS: five detours, float ABI, HP correlation, nested TLS, filters, overflow, JSON, world positions, stale-handle/UID/class rejection");return 0;
}
