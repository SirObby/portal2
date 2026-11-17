#!/usr/bin/env python3
"""Losslessly remux SAR renders for ordinary MP4 decoding and rebind sidecars.

SAR's H.264 packets can have zero duration on the final frame. MP4's edit list
then excludes that frame. Read all packets, explicitly set 1/fps video packet
length, and disable edit lists. Keep the original render and sidecar untouched.
Training output contains video only. Verify decoded RGB frame hashes before
accepting the transformed sidecar, then run the full alignment validator.
"""
import argparse
import json
from pathlib import Path
import subprocess

from render_alignment import read_mapping, digest, validate, require


def pixel_hashes(path, ignore_editlist=False):
    args=['ffmpeg','-v','error']
    if ignore_editlist: args += ['-ignore_editlist','1']
    args += ['-i',str(path),'-map','0:v:0','-an','-fps_mode','passthrough',
             '-pix_fmt','rgb24','-f','framemd5','-']
    result=subprocess.run(args,check=True,capture_output=True,text=True)
    return [line.rsplit(',',1)[-1].strip() for line in result.stdout.splitlines()
            if line and not line.startswith('#')]


def normalize(source, output, demo):
    sidecar=Path(str(source)+'.alignment.jsonl')
    header,frames,footer=read_mapping(sidecar)
    require(digest(source)==footer['video_sha256'],'Source video hash mismatch')
    require(digest(demo)==header['demo_sha256'],'Source demo hash mismatch')
    output.parent.mkdir(parents=True,exist_ok=True)
    temporary=output.with_name(output.stem+'.partial.mp4')
    fps=header['fps']
    subprocess.run(['ffmpeg','-v','error','-y','-ignore_editlist','1','-i',str(source),
                    '-map','0:v:0','-c','copy','-bsf:v',f'setts=duration=1/{fps}/TB',
                    '-use_editlist','0',str(temporary)],check=True)
    original_hashes=pixel_hashes(source,True)
    normalized_hashes=pixel_hashes(temporary)
    require(len(original_hashes)==len(frames),'Source decoded image count disagrees with sidecar')
    require(original_hashes==normalized_hashes,'Remux changed decoded RGB images')
    transformed=dict(header, demo=str(demo.resolve()), normalization=dict(
        source_video=str(source.resolve()),source_video_sha256=digest(source),
        source_sidecar_sha256=digest(sidecar),decoded_rgb_frames_identical=True,
        operation='copy video packets; duration=1/fps; no edit list; omit audio'))
    final=dict(footer,video_sha256=digest(temporary))
    mapped=Path(str(output)+'.alignment.jsonl')
    partial=Path(str(mapped)+'.partial')
    partial.write_text(''.join(json.dumps(row,separators=(',',':'))+'\n'
                               for row in [transformed,*frames,final]))
    temporary.replace(output)
    partial.replace(mapped)
    try:
        _,_,report=validate(output,mapped,demo)
    except Exception:
        mapped.replace(partial)
        raise
    return report


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory',type=Path)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--suffix',default='.bc-v1.mp4')
    args=p.parse_args()
    results=[]
    for demo in sorted(args.directory.glob('*.dem')):
        report=normalize(demo.with_suffix(args.suffix),args.output/(demo.stem+'.mp4'),demo)
        results.append(report)
        print(json.dumps(dict(demo=demo.name,frames=report['frame_count'],validated=True)),flush=True)
    (args.output/'validation.json').write_text(json.dumps(results,indent=2)+'\n')


if __name__=='__main__': main()
