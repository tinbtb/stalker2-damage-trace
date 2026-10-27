"""Assert semantic results from the real native-detour fixture."""
from pathlib import Path
import copy
import csv,json,math
from analyze_trace import analyze,check_armor
R=Path(__file__).resolve().parent
summary=analyze(R/'build/selftest.jsonl',R/'build/selftest_analysis')
assert summary['armor_checks']=={'match':5}
assert summary['observed_kills']==1 and summary['malformed_lines']==0
rows=list(csv.DictReader((R/'build/selftest_analysis/armor_checks.csv').open()))
assert all(r['rng_state']=='match' and float(r['error'])==0 for r in rows)
hits=list(csv.DictReader((R/'build/selftest_analysis/hits.csv').open()))
assert any(float(r['post_armor_difference'])<0 for r in hits if r['post_armor_difference'])
assert any(r.get('separation_m')=='5.0' for r in hits)
assert any(not r.get('separation_m') for r in hits)
kills=list(csv.DictReader((R/'build/selftest_analysis/observed_kills.csv').open()))
assert len(kills)==1 and kills[0]['observed_native_hits']=='1'
# Deliberately corrupt the observed armor result and the RNG snapshot: neither
# may be silently reported as a confirmed formula match / confirmed RNG draw.
events=[json.loads(s) for s in (R/'build/selftest.jsonl').read_text().splitlines()]
stage=dict(next(e for e in events if e['armor'])['armor'][0])
stage['result']+=1;assert check_armor(stage)['state']=='mismatch'
stage['seed_after']^=1;assert check_armor(stage)['rng_state']=='seed_ambiguous'
stage['reads']=0;assert check_armor(stage)['state']=='missing_reads'
# Inter-hit HP recovery must remain in the same observed kill sequence.
fixture=R/'build/recovery_fixture.jsonl'
base=next(e for e in events if e['armor'])
recovery=[]
for i,(before,after) in enumerate([(200,100),(101,1),(2,0)],101):
    e=copy.deepcopy(base);e.update(id=i,parent=0,qpc_begin=i,qpc_end=i,hp_before=before,hp_after=after,hp_bound_14c=200)
    recovery.append(e)
recovery.append(dict(event='heartbeat',dropped=0))
fixture.write_text('\n'.join(json.dumps(e) for e in recovery)+'\n')
analyze(fixture,R/'build/recovery_analysis')
observed=list(csv.DictReader((R/'build/recovery_analysis/observed_kills.csv').open()))
assert len(observed)==1 and observed[0]['observed_native_hits']=='3' and float(observed[0]['inter_hit_hp_gain'])==2
print('PASS: exact float32 comparison, RNG verification, mismatch detection, post-armor changes, observed lethal count and incomplete-capture labeling')
