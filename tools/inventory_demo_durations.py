#!/usr/bin/env python3
"""Read only Source demo headers to inventory selected map folders, not replay them."""
import argparse
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path
import struct


def inventory(root):
    rows=[]; errors=[]; groups=defaultdict(list)
    for path in sorted(root.rglob('*.dem')):
        try:
            with path.open('rb') as source: header=source.read(1072)
            if len(header)!=1072 or header[:8]!=b'HL2DEMO\0': raise ValueError('Invalid demo header')
            protocol,network=struct.unpack_from('<ii',header,8)
            seconds,ticks,frames,signon=struct.unpack_from('<fiii',header,1056)
            map_name=header[536:796].split(b'\0',1)[0].decode('utf-8','replace')
            flags=[]
            if not math.isfinite(seconds) or seconds<=0: flags.append('invalid_duration')
            if ticks<=0: flags.append('invalid_ticks')
            rate=ticks/seconds if math.isfinite(seconds) and seconds>0 else None
            if rate is not None and not 59.5<=rate<=60.5: flags.append('header_tick_rate_not_near_60')
            if map_name!=path.parent.name:flags.append('map_folder_mismatch')
            row=dict(path=str(path.resolve()),name=path.name,map=map_name,bytes=path.stat().st_size,
                     demo_protocol=protocol,network_protocol=network,
                     playback_seconds=seconds if math.isfinite(seconds) else None,
                     playback_ticks=ticks,playback_frames=frames,header_tick_rate=rate,
                     header_sha256=hashlib.sha256(header).hexdigest(),flags=flags)
            rows.append(row); groups[path.parent.name].append(row)
        except (OSError,ValueError,struct.error) as exc: errors.append(dict(path=str(path),error=str(exc)))
    positive=lambda items:sum(r['playback_seconds'] for r in items if r['playback_seconds'] is not None and r['playback_seconds']>0)
    return dict(root=str(root.resolve()),scope='1072-byte headers and file sizes only; no replay or action/entity parsing',
                demo_count=len(rows),map_folder_count=len(groups),total_positive_header_seconds=positive(rows),
                total_positive_header_hours=positive(rows)/3600,
                total_ticks=sum(r['playback_ticks'] for r in rows if r['playback_ticks']>0),
                seconds_from_ticks_at_60=sum(r['playback_ticks'] for r in rows if r['playback_ticks']>0)/60,
                total_source_bytes=sum(r['bytes'] for r in rows),flagged_demos=[r for r in rows if r['flags']],
                maps=[dict(map=name,count=len(items),header_seconds=positive(items),
                           ticks=sum(r['playback_ticks'] for r in items if r['playback_ticks']>0),
                           source_bytes=sum(r['bytes'] for r in items)) for name,items in sorted(groups.items())],
                errors=errors,demos=rows)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory',type=Path)
    p.add_argument('--output',type=Path,required=True)
    args=p.parse_args(); report=inventory(args.directory)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k not in ('demos','maps','flagged_demos')},indent=2))
    print('Flagged headers:',len(report['flagged_demos']))
    return bool(report['errors'])


if __name__=='__main__':raise SystemExit(main())
