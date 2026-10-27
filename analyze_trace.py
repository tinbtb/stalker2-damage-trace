"""Compare observed native hit stages; never substitute expected HTK for a kill."""
import argparse,csv,ctypes,json,math,struct
from pathlib import Path
f32=lambda x:ctypes.c_float(x).value
bits=lambda x:struct.pack('<f',x)
crt=ctypes.CDLL('ucrtbase');crt.powf.argtypes=[ctypes.c_float,ctypes.c_float];crt.powf.restype=ctypes.c_float
def check_armor(s):
    s=dict(s)
    for key in ['damage_in','armor','coefficient','ap','result','human_coef','mutant_coef','min_chance','max_chance']:
        if s.get(key) is not None:s[key]=f32(s[key])
    required=1|2|4|8|16|32|64|512|1024
    if s['reads']&required!=required:return dict(state='missing_reads')
    if any(s.get(k) is None for k in ['damage_in','armor','coefficient','ap','result','human_coef','mutant_coef','min_chance','max_chance']):return dict(state='nonfinite')
    if s['category_raw']!=6:return dict(state='unsupported_category')
    uid=s['attacker_uid'];source=s['source_raw']
    special=(uid!=0xfffffffe and uid<=0xffffffef and (source in (4,11,12) or uid&0xf8000000==0x08000000))
    if special and not s['skip_armor']:return dict(state='special_source_branch')
    if s['skip_armor']:expected=s['damage_in']
    else:
        delta=f32(s['ap']-s['armor'])
        factor=crt.powf(s['coefficient'],delta) if s['ap']!=0 else 1.
        # Reserved source identifiers can bypass the deflection multiplier.
        k=(s['human_coef'] if s['human'] else s['mutant_coef']) if s['deflected'] and uid<=0xffffffef else 1.
        expected=f32(f32(s['damage_in']*k)*factor)
    result=dict(state='match' if bits(expected)==bits(s['result']) else 'mismatch',predicted=expected,actual=s['result'],error=s['result']-expected)
    if s['roll_calls']==1 and s['reads']&(128|256)==(128|256):
        if uid>0x07ffffff or source==4:
            result['rng_state']='no_rng_expected' if s['seed_before']==s['seed_after'] and not s['roll_result'] else 'unexpected'
        elif s['seed_after']==(s['seed_before']*196314165+907633515)&0xffffffff:
            roll=(s['seed_after']>>9)/8388608.
            threshold=max(min(f32(f32(s['ap']-s['armor'])+1),s['max_chance']),s['min_chance'])
            result.update(roll=roll,threshold=threshold,rng_state='match' if int(roll>threshold)==s['roll_result']==s['deflected'] else 'mismatch')
        else:result['rng_state']='seed_ambiguous'
    else:result['rng_state']='not_observed'
    return result

def analyze(path,out):
    out.mkdir(exist_ok=True,parents=True);events=[];hits=[];truncated=0;malformed=0
    for line in path.read_text(encoding='utf-8-sig').splitlines():
        try:events.append(json.loads(line))
        except json.JSONDecodeError:malformed+=1
    sessions=[e for e in events if e.get('event')=='session']
    drops=max((e.get('dropped',0) for e in events if e.get('event')=='heartbeat'),default=None)
    data=sorted((e for e in events if e.get('event')=='hit'),key=lambda x:x['qpc_begin'])
    nested_parents={e['parent'] for e in data if e['parent']}
    armors=[];kills=[];series={}
    for h in data:
        for key in ['hp_before','hp_after','hp_bound_14c']:
            if h.get(key) is not None:h[key]=f32(h[key])
        for stage in h['health']:
            for key in ['requested_delta','current','result']:
                if stage.get(key) is not None:stage[key]=f32(stage[key])
        for stage in h['armor']:
            if stage.get('result') is not None:stage['result']=f32(stage['result'])
        if h['stage_overflow']:truncated+=1
        if h['armor_count']!=len(h['armor']) or h['bone_count']!=len(h['bones']) or h['health_count']!=len(h['health']):truncated+=1
        matching=[s for s in h['health'] if s['matching_target']]
        requested=sum(-s['requested_delta'] for s in matching)
        hp_valid=h['read_mask']&(4|512)==(4|512) and h['hp_before'] is not None and h['hp_after'] is not None
        loss=h['hp_before']-h['hp_after'] if hp_valid else None
        last_armor=h['armor'][-1]['result'] if h['armor'] else None
        adjustment=requested-last_armor if last_armor is not None and matching else None
        hits.append(dict(id=h['id'],label=h['label'],victim_uid=h['victim_uid'],attacker_uid=h['attacker_uid'],parent=h['parent'],contains_nested_hit=h['id'] in nested_parents,hp_before=h['hp_before'],hp_after=h['hp_after'],actual_hp_loss=loss,requested_damage=requested if matching else None,last_armor_damage=last_armor,post_armor_difference=adjustment,bones=','.join(str(b['bone_raw']) for b in h['bones']),deflected=any(s['deflected'] for s in h['armor']),read_mask=h['read_mask'],stage_overflow=h['stage_overflow']))
        separation=h.get('separation_cm')
        valid_distance=h.get('distance_read_mask',0)&3==3 and separation is not None and math.isfinite(separation) and separation>=0
        hits[-1].update(separation_m=separation/100 if valid_distance else None,distance_basis=h.get('distance_basis','not_captured'),projectile_travel_cm=h.get('projectile_travel_cm'))
        for i,s in enumerate(h['armor']):armors.append(dict(hit_id=h['id'],stage=i,label=h['label'],**check_armor(s)))
        # Count native hit events, not trigger pulls. Pellets can be separate hits.
        if not hp_valid or h['id'] in nested_parents:continue
        key=(h['victim_ptr'],h['victim_uid'],h['label'])
        active=series.get(key)
        # Small inter-hit recovery must not turn a three-hit kill into a new
        # one-hit sequence. Retain it explicitly; a return to the HP bound starts
        # a fresh sequence and remains distinguishable from a clean capture.
        reset=active is not None and h['hp_before']>active['last_hp']+1e-4 and h['hp_bound_14c']>0 and h['hp_before']>=h['hp_bound_14c']-1e-4
        if active is None or reset:
            active=dict(first_id=h['id'],initial_hp=h['hp_before'],hp_bound=h['hp_bound_14c'],hits=0,last_hp=h['hp_before'],attackers=set(),deflections=0,inter_hit_hp_gain=0.,inter_hit_hp_loss=0.)
            series[key]=active
        change=h['hp_before']-active['last_hp']
        if change>1e-4:active['inter_hit_hp_gain']+=change
        elif change<-1e-4:active['inter_hit_hp_loss']-=change
        active['hits']+=1;active['last_hp']=h['hp_after'];active['attackers'].add(h['attacker_uid']);active['deflections']+=sum(bool(s['deflected']) for s in h['armor'])
        if h['hp_before']>0 and h['hp_after']<=0:
            kills.append(dict(label=h['label'],victim_uid=h['victim_uid'],first_id=active['first_id'],last_id=h['id'],observed_native_hits=active['hits'],initial_hp=active['initial_hp'],hp_bound=active['hp_bound'],starts_at_hp_bound=abs(active['initial_hp']-active['hp_bound'])<1e-4,single_attacker=len(active['attackers'])==1,deflected_events=active['deflections'],inter_hit_hp_gain=active['inter_hit_hp_gain'],inter_hit_hp_loss=active['inter_hit_hp_loss'],capture_integrity='no_reported_drops' if drops==0 and not malformed and not truncated else 'incomplete_or_unknown'))
            del series[key]
    def write(name,rows):
        if not rows:return
        keys=list(dict.fromkeys(k for row in rows for k in row))
        with (out/name).open('w',newline='',encoding='utf8') as f:
            w=csv.DictWriter(f,fieldnames=keys);w.writeheader();w.writerows(rows)
    write('hits.csv',hits);write('armor_checks.csv',armors);write('observed_kills.csv',kills)
    states={s:sum(r['state']==s for r in armors) for s in sorted({r['state'] for r in armors})}
    summary=dict(hit_events=len(data),observed_kills=len(kills),armor_checks=states,distance_samples=sum(r['separation_m'] is not None for r in hits),reported_drops=drops,malformed_lines=malformed,stage_truncations=truncated,session_headers=len(sessions),unmatched_live_sequences=len(series),notes=['Observed kills count native damage events, not bullets fired.','Separation is capsule-centre distance at impact, not projectile travel distance.','hp_bound_14c is a recorded engine field; starts_at_hp_bound is not proof of a clean controlled test.','Healing, mixed attackers, missed hits, reloads and missing events can invalidate HTK.','Raw UID and bone values are not resolved prototype or item names.'])
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');return summary

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('trace',type=Path);p.add_argument('--out',type=Path)
    args=p.parse_args();print(json.dumps(analyze(args.trace,args.out or args.trace.with_suffix('')),indent=2))
