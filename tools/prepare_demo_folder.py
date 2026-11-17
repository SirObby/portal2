#!/usr/bin/env python3
"""Prepare a map folder: playback/capture, render, normalize, export, audit.

Linux/Steam + installed SAR alignment build. Resumes only hash-verified outputs.
One game runs sar_startdemosfolder; native samples carry their demo identity.
Scientific training admission remains explicit and separate from processing success.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import signal
import subprocess
import time

from export_demonstrations import export
from normalize_demo_video import normalize
from render_alignment import digest, require, validate

ROOT=Path(__file__).resolve().parents[1]


def console_value(value):
    require(not any(c in str(value) for c in '\n\r";'), 'Unsafe Source console argument')
    return '"'+str(value)+'"'


def save(path, value):
    partial=path.with_suffix(path.suffix+'.partial')
    partial.write_text(json.dumps(value,indent=2)+'\n')
    partial.replace(path)


def receive_until(sock, predicate, process, log, timeout=120):
    deadline=time.monotonic()+timeout
    history=''
    sock.settimeout(.25)
    while time.monotonic()<deadline:
        if predicate(history): return history
        require(process.poll() is None,'Game exited before stage completion')
        try:
            data=sock.recv(65536)
            require(bool(data),'Netconsole disconnected')
            text=data.decode('utf-8','replace')
            log.write(text); log.flush()
            history=(history+text)[-100000:]
        except socket.timeout: pass
    raise TimeoutError('Timed out waiting for game stage')


def playback(demo, capture, game, runtime, plugin, config, port, suffix, folder):
    # An existing listener belongs to another run; never commandeer it.
    try:
        with socket.create_connection(('127.0.0.1',port),.3):
            raise ValueError(f'Netconsole port {port} already in use')
    except (ConnectionRefusedError, socket.timeout): pass
    is_folder=demo.is_dir()
    env=dict(os.environ,PORTAL2_RL_DEMO_CAPTURE=str(capture),PORTAL2_RL_DEMO_SHA256='' if is_folder else digest(demo))
    command=[str(runtime),str(game/'portal2.sh'),'-game','portal2','-steam','-vulkan',
             '-windowed','-w','256','-h','192','-novid','-netconport',str(port)]
    inputs=sorted(demo.glob('*.dem')) if is_folder else [demo]
    sources={p.name:p.with_suffix(suffix) for p in inputs}
    sidecars={name:Path(str(video)+'.alignment.jsonl') for name,video in sources.items()}
    previous={name:p.stat().st_mtime_ns if p.exists() else None for name,p in sidecars.items()}
    finished=set()
    def rendered(_history):
        for name,path in sidecars.items():
            if name not in finished and path.exists() and path.stat().st_mtime_ns!=previous[name]:
                finished.add(name)
                print(json.dumps(dict(demo=name,stage='render_finalized',index=len(finished),total=len(inputs))),flush=True)
        return len(finished)==len(inputs)
    sock=None
    with (folder/'game.log').open('w') as game_log, (folder/'console.log').open('w') as console_log:
        process=subprocess.Popen(command,env=env,stdout=game_log,stderr=subprocess.STDOUT,start_new_session=True)
        try:
            deadline=time.monotonic()+120
            while time.monotonic()<deadline:
                require(process.poll() is None,'Game failed to launch; inspect game.log')
                try:
                    sock=socket.create_connection(('127.0.0.1',port),.5); break
                except (ConnectionRefusedError,socket.timeout): time.sleep(.25)
            require(sock is not None,'Game netconsole did not become ready')
            # Resolve the engine's original client-state vtable before SAR wraps
            # it. SAR subsequently chains our input hook through its own hook.
            sock.sendall(('plugin_load '+console_value(plugin.with_suffix(''))+'\n').encode())
            receive_until(sock,lambda h:'Loaded plugin' in h and str(plugin.with_suffix('')) in h,
                          process,console_log)
            require(capture.exists(),'Native capture plugin did not start')
            sock.sendall(('exec '+console_value(config)+'\n').encode())
            receive_until(sock,lambda h:'Loaded SourceAutoRecord' in h,process,console_log)
            command='sar_startdemosfolder' if is_folder else 'playdemo'
            sock.sendall(('sar_render_autostart_extension '+suffix.lstrip('.')+'\n'+command+' '+console_value(demo)+'\n').encode())
            receive_until(sock,rendered,process,console_log,timeout=180*len(inputs))
            sock.sendall(b'quit\n')
            process.wait(timeout=30)
            require(process.returncode==0, f'Game exited with {process.returncode}')
        finally:
            if sock is not None: sock.close()
            if process.poll() is None:
                os.killpg(process.pid,signal.SIGTERM)
                try: process.wait(timeout=10)
                except subprocess.TimeoutExpired: os.killpg(process.pid,signal.SIGKILL); process.wait()
    return sources if is_folder else sources[demo.name]



def split_folder_capture(journal, demos, output):
    """Derive per-demo journals without losing original global sample indices."""
    with journal.open() as source:
        lines=[json.loads(line) for line in source]
    require(len(lines)>=2, 'Empty folder capture')
    header,*samples,footer=lines
    require(header.get('schema')=='portal2-client-demo-capture-v1', 'Invalid folder capture schema')
    require(footer.get('type')=='end' and footer.get('complete') is True and footer.get('command_count')==len(samples),
            'Incomplete folder native capture')
    by_name={demo.name:[] for demo in demos}
    for index,sample in enumerate(samples):
        require(sample.get('index')==index, 'Folder capture index gap')
        name=Path(sample.get('demo_name','')).name
        if not name.endswith('.dem'): name += '.dem'
        require(name in by_name, 'Capture sample belongs to an unexpected demo: '+name)
        selected=by_name[name]
        selected.append(dict(sample,index=len(selected),source_native_index=index))
    source_hash=digest(journal)
    for demo in demos:
        selected=by_name[demo.name]
        require(bool(selected),'No native samples captured for '+demo.name)
        folder=output/demo.stem
        folder.mkdir(parents=True,exist_ok=True)
        derived=dict(header,demo_sha256=digest(demo),source_folder_capture=str(journal.resolve()),
                     source_folder_capture_sha256=source_hash,demo_name=demo.name)
        rows=[derived,*selected,dict(type='end',complete=True,command_count=len(selected))]
        target=folder/'capture.jsonl'
        temporary=folder/'capture.jsonl.partial'
        temporary.write_text(''.join(json.dumps(row,separators=(',',':'))+'\n' for row in rows))
        temporary.replace(target)


def run(args):
    started=time.monotonic()
    timings={}
    demos=sorted(args.directory.resolve().glob('*.dem'))
    require(bool(demos),'No .dem files found in input directory')
    if args.limit: demos=demos[:args.limit]
    args.output=args.output.resolve(); args.output.mkdir(parents=True,exist_ok=True)
    settings=dict(plugin_sha256=digest(args.plugin),sar_sha256=digest(args.game/'portal2/sar.so'),
                  config_sha256=digest(args.game/'portal2/cfg'/args.config),
                  pipeline_sha256=digest(Path(__file__)),
                  exporter_sha256=digest(ROOT/'tools/export_demonstrations.py'),
                  capture_reader_sha256=digest(ROOT/'tools/demo_capture.py'),
                  normalizer_sha256=digest(ROOT/'tools/normalize_demo_video.py'))
    results={}; pending=[]
    for demo in demos:
        folder=args.output/demo.stem; folder.mkdir(exist_ok=True)
        identity=dict(demo_sha256=digest(demo),**settings)
        status=folder/'status.json'
        if args.resume and status.exists():
            old=json.loads(status.read_text())
            if old.get('complete') and old.get('identity')==identity:
                try:
                    require(digest(folder/'episode.jsonl')==old['episode_sha256'],'Export hash changed')
                    require(digest(folder/'capture.jsonl')==old['capture_sha256'],'Capture hash changed')
                    validate(folder/'video.mp4',folder/'video.mp4.alignment.jsonl',demo)
                    results[demo.name]=old
                    print(json.dumps(dict(demo=demo.name,stage='resumed')),flush=True)
                    continue
                except (OSError,ValueError,KeyError): pass
        row=dict(demo=str(demo),identity=identity,complete=False,training_ready=False)
        save(status,row); results[demo.name]=row; pending.append(demo)
    def batch():
        save(args.output/'manifest.json',dict(schema='portal2-folder-preparation-v2',
             input=str(args.directory.resolve()),playback_strategy='one_game_sar_startdemosfolder',
             results=[results[d.name] for d in demos],training_ready=False,
             elapsed_seconds=time.monotonic()-started,timings=timings))
    batch()
    if pending:
        # A resume subset uses a queue of symlinks so SAR does not replay good
        # episodes. A complete folder run uses the requested folder directly.
        queue=args.directory.resolve()
        if len(pending)!=len(list(queue.glob('*.dem'))):
            queue=args.output/'pending-demo-queue'
            queue.mkdir(exist_ok=True)
            for existing in queue.glob('*.dem'):
                require(existing.is_symlink(),'Refusing to replace a non-symlink queue entry')
                existing.unlink()
            for demo in pending: (queue/demo.name).symlink_to(demo)
        plan={demo.name:digest(demo) for demo in pending}
        save(args.output/'capture-plan.json',dict(demos=plan,identity=settings))
        partial=args.output/'folder.capture.jsonl.partial'
        try:
            print(json.dumps(dict(stage='folder_playback_capture_render',demos=len(pending),
                                  command='sar_startdemosfolder',queue=str(queue))),flush=True)
            phase_started=time.monotonic()
            sources=playback(queue,partial,args.game,args.runtime,args.plugin,args.config,args.port,
                             '.prepared-folder-'+settings['plugin_sha256'][:10]+'.mp4',args.output)
            timings['launch_playback_capture_render_shutdown_seconds']=time.monotonic()-phase_started
            journals=args.output/'folder-captures'
            journals.mkdir(exist_ok=True)
            journal=journals/(digest(partial)+'.jsonl')
            partial.replace(journal)
            save(args.output/'capture-latest.json',dict(path=str(journal),sha256=digest(journal)))
            split_folder_capture(journal,pending,args.output)
        except Exception as exc:
            for demo in pending:
                results[demo.name]['error']=f'{type(exc).__name__}: {exc}'
                save(args.output/demo.stem/'status.json',results[demo.name])
            batch()
            raise
        phase_started=time.monotonic()
        for number,demo in enumerate(pending,1):
            row=results[demo.name]; folder=args.output/demo.stem
            try:
                print(json.dumps(dict(demo=demo.name,stage='normalize_validate_export',
                                      index=number,total=len(pending))),flush=True)
                report=normalize(sources[demo.name],folder/'video.mp4',demo)
                result=export(demo,folder/'episode.jsonl',folder/'video.mp4',capture=folder/'capture.jsonl')
                row.update(complete=True,export=result,video_validation=report,
                           capture_sha256=digest(folder/'capture.jsonl'),episode_sha256=digest(folder/'episode.jsonl'),
                           training_exclusions=['causal_simulation_alignment_unverified','gym_actuator_equivalence_unverified'])
            except Exception as exc:
                row['error']=f'{type(exc).__name__}: {exc}'
                print(json.dumps(dict(demo=demo.name,stage='failed',error=row['error'])),flush=True)
            save(folder/'status.json',row); batch()
        timings['normalize_pixel_verify_align_export_seconds']=time.monotonic()-phase_started
    batch()
    return int(any(not row['complete'] for row in results.values()))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--resume',action='store_true')
    parser.add_argument('--limit',type=int,help='Optional pilot subset; default processes the entire folder')
    parser.add_argument('--port',type=int,default=27666)
    parser.add_argument('--config',default='portal2_rl_render.cfg')
    parser.add_argument('--plugin',type=Path,default=ROOT/'cpp/build/portal2_rl_plugin.so')
    parser.add_argument('--game',type=Path,default=Path.home()/'.steam/steam/steamapps/common/Portal 2')
    parser.add_argument('--runtime',type=Path,default=Path.home()/'.local/share/Steam/steamapps/common/SteamLinuxRuntime/steam-runtime/run.sh')
    args=parser.parse_args()
    require(args.limit is None or args.limit>0,'--limit must be positive')
    return run(args)


if __name__=='__main__': raise SystemExit(main())
