"""Validate and index the read-only client plugin's per-demo command journal."""
from collections import defaultdict
import json
import math
from render_alignment import require, digest


def state_vector(state):
    if state is None:
        return [None]*18, [False]*18
    values = [*state['pos'], *state['vel'], *state['ang'], int(state['on_ground']),
              int(state['portal1']['placed']), *state['portal1']['pos'],
              int(state['portal2']['placed']), *state['portal2']['pos']]
    require(len(values)==18 and all(type(v) in (int,float) and math.isfinite(v) for v in values),
            'Invalid client state vector')
    valid=[True]*18
    if not state['portals_valid']:
        valid[10:]=[False]*8
        values[10:]=[None]*8
    return values,valid


def read_capture(path, demo_hash, records):
    with path.open() as source:
        lines=[json.loads(line) for line in source]
    require(len(lines)>=2, 'Empty capture journal')
    header,*samples,footer=lines
    require(header.get('schema')=='portal2-client-demo-capture-v1','Invalid capture schema')
    require(header.get('demo_sha256')==demo_hash,'Capture demo hash mismatch')
    require(footer.get('type')=='end' and footer.get('complete') is True,'Incomplete native capture')
    require(footer.get('command_count')==len(samples),'Capture command count mismatch')
    source={(r['slot'],r['command_index']):r['usercmd'] for r in records if 'usercmd' in r}
    # Playback-fix restarts produce an initial incomplete pass. Select the final
    # monotonically increasing command pass explicitly, never merge its states
    # with the abandoned initial pass.
    final_start=0
    previous={}
    for i,s in enumerate(samples):
        require(s.get('type')=='command' and s.get('index')==i,'Capture index gap')
        slot=s['slot']; seq=s['sequence']
        if slot in previous and seq<=previous[slot]: final_start=i; previous={}
        previous[slot]=seq
    indexed=defaultdict(list)
    rejected=0
    for s in samples[final_start:]:
        key=(s['slot'],s['sequence'])
        require(key in source, 'Native capture contains a command absent from the source demo')
        cmd=source[key]
        require(len(s['view_angles'])==3 and len(s['movement'])==3,'Invalid native command dimensions')
        require(s['command_number']==cmd['command_number'] and s['command_tick']==cmd['tick_count']
                and s['buttons']==cmd['buttons'], 'Native/raw command identity mismatch')
        for left,right in zip(s['view_angles']+s['movement'],
                              cmd['view_angles']+[cmd['forward_move'],cmd['side_move'],cmd['up_move']]):
            require(math.isclose(left,right,rel_tol=1e-6,abs_tol=1e-5),'Native/raw command float mismatch')
        values,valid=state_vector(s['state'])
        if s['skipping'] or s['paused']:
            rejected+=1
            continue
        indexed[key].append(dict(state=values,state_valid=valid,raw_state=s['state'],
            source='replayed_client_entities',stage=s['stage'],playback_tick=s['playback_tick'],
            command_number=s['command_number'],command_tick=s['command_tick'],native_index=s['index'],
            source_native_index=s.get('source_native_index',s['index']),
            causal_simulation_alignment_verified=False))
    return indexed,dict(path=str(path.resolve()),sha256=digest(path),
                        captured_commands=len(samples),final_pass_start=final_start,
                        retained_samples=sum(map(len,indexed.values())),excluded_paused_or_skipping=rejected,
                        action_decoding_verified=True,causal_simulation_alignment_verified=False)
