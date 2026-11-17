#!/usr/bin/env python3
"""Export protocol-4 Portal 2 demos to auditable per-record-tick JSONL.

No game launch, rendering, interpolation, clipping, or fabricated entity state.
Optional videos must pass render_alignment.validate before frame references join.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import struct
import sys

from render_alignment import validate, require, digest
from demo_capture import read_capture

ACTION_FIELDS = ['move_forward', 'move_side', 'pitch_delta', 'yaw_delta',
                 'jump', 'duck', 'use', 'fire_portal1', 'fire_portal2']
STATE_FIELDS = ['pos.x', 'pos.y', 'pos.z', 'vel.x', 'vel.y', 'vel.z',
                'ang.pitch', 'ang.yaw', 'ang.roll', 'on_ground', 'portal1.placed',
                'portal1.pos.x', 'portal1.pos.y', 'portal1.pos.z', 'portal2.placed',
                'portal2.pos.x', 'portal2.pos.y', 'portal2.pos.z']


class Bits:
    def __init__(self, data):
        self.data, self.offset = data, 0

    def read(self, count):
        require(self.offset + count <= len(self.data) * 8, 'Truncated usercmd')
        result = 0
        for i in range(count):
            result |= ((self.data[self.offset // 8] >> (self.offset % 8)) & 1) << i
            self.offset += 1
        return result

    def optional(self, count, default=0):
        return self.read(count) if self.read(1) else default

    def floating(self):
        value = struct.unpack('<f', struct.pack('<I', self.optional(32)))[0]
        require(math.isfinite(value), 'Nonfinite usercmd float')
        return value


def decode_usercmd(data):
    """Demo commands are encoded against a zero CUserCmd, not the last record.

    Omitted sequence/tick counters increment the zero baseline to one. Other
    omitted fields remain zero. Preserve extension bits for Portal 2 auditing.
    """
    bits = Bits(data)
    cmd = dict(command_number=bits.optional(32, 1), tick_count=bits.optional(32, 1))
    cmd['view_angles'] = [bits.floating() for _ in range(3)]
    cmd['forward_move'], cmd['side_move'], cmd['up_move'] = [bits.floating() for _ in range(3)]
    cmd['buttons'] = bits.optional(32)
    cmd['impulse'] = bits.optional(8)
    weapon_present = bits.read(1)
    cmd['weapon_select'] = bits.read(11) if weapon_present else 0
    cmd['weapon_subtype'] = bits.optional(6) if weapon_present else 0
    for name in ('mouse_dx', 'mouse_dy'):
        value = bits.optional(16)
        cmd[name] = value - 65536 if value >= 32768 else value
    cmd['decoded_bits'] = bits.offset
    cmd['remaining_bits'] = len(data) * 8 - bits.offset
    cmd['payload_hex'] = data.hex()
    return cmd


def read_demo(path):
    raw = path.read_bytes()
    require(len(raw) >= 1072 and raw[:8] == b'HL2DEMO\0', 'Invalid demo header')
    protocol, network = struct.unpack_from('<ii', raw, 8)
    require(protocol == 4, 'Only demo protocol 4 is supported')
    strings = [raw[i:i+260].split(b'\0', 1)[0].decode('utf-8', 'replace')
               for i in (16, 276, 536, 796)]
    seconds, ticks, frames, signon = struct.unpack_from('<fiii', raw, 1056)
    require(math.isfinite(seconds) and seconds >= 0 and ticks >= 0, 'Invalid header duration')
    header = dict(demo=str(path.resolve()), demo_sha256=hashlib.sha256(raw).hexdigest(),
                  demo_protocol=protocol, network_protocol=network, server=strings[0],
                  player=strings[1], map=strings[2], game=strings[3],
                  playback_seconds=seconds, playback_ticks=ticks, playback_frames=frames)
    records = []
    pos = 1072

    def take(size):
        nonlocal pos
        require(size >= 0 and pos + size <= len(raw), f'Truncated demo at byte {pos}')
        result = raw[pos:pos+size]
        pos += size
        return result

    while True:
        offset = pos
        kind = take(1)[0]
        if kind == 7:
            header['stop_offset'] = offset
            break
        tick, slot = struct.unpack('<iB', take(5))
        row = dict(kind=kind, tick=tick, slot=slot, byte_offset=offset)
        if kind in (1, 2):
            cameras = []
            for _ in range(2):
                values = struct.unpack('<I18f', take(76))
                require(all(math.isfinite(x) for x in values), 'Nonfinite packet camera')
                cameras.append(dict(flags=values[0], view_origin=values[1:4],
                                    view_angles=values[4:7], local_view_angles=values[7:10],
                                    view_origin2=values[10:13], view_angles2=values[13:16],
                                    local_view_angles2=values[16:19]))
            row['camera'] = cameras
            row['in_sequence'], row['out_sequence'] = struct.unpack('<ii', take(8))
        elif kind == 3:
            records.append(row)
            continue
        elif kind in (5, 8):
            row['command_index' if kind == 5 else 'callback_index'], = struct.unpack('<i', take(4))
        else:
            require(kind in (4, 6, 9), f'Unknown demo command {kind} at {offset}')
        length, = struct.unpack('<i', take(4))
        payload = take(length)
        if kind == 5:
            row['usercmd'] = decode_usercmd(payload)
        elif kind == 4:
            row['console_command'] = payload.rstrip(b'\0').decode('utf-8', 'replace')
        records.append(row)
    header['message_counts'] = dict(Counter(r['kind'] for r in records))
    return header, records


def action_candidate(cmd, previous, scale):
    contiguous = previous is not None and cmd['command_number'] == previous['command_number'] + 1
    pitch = cmd['view_angles'][0] - previous['view_angles'][0] if contiguous else None
    yaw = ((cmd['view_angles'][1] - previous['view_angles'][1] + 180) % 360 - 180) if contiguous else None
    buttons = cmd['buttons']
    vector = [cmd['forward_move'] / scale, cmd['side_move'] / scale, pitch, yaw,
              *[int(bool(buttons & (1 << bit))) for bit in (1, 2, 5, 0, 11)]]
    outside = [ACTION_FIELDS[i] for i, (v, limit) in enumerate(zip(vector, [1, 1, 10, 10, 1, 1, 1, 1, 1]))
               if v is not None and abs(v) > limit]
    return dict(vector=vector, in_gym_bounds=not outside and None not in vector,
                outside_bounds=outside, angle_delta_has_contiguous_predecessor=contiguous,
                semantics='recorded held buttons; movement divided by explicit scale; not actuator-equivalent')


def export(path, output, video=None, scale=175.0, capture=None):
    header, records = read_demo(path)
    capture_index, capture_report = read_capture(capture, header['demo_sha256'], records) if capture else ({}, None)
    by_tick = defaultdict(list)
    for record in records:
        by_tick[record['tick']].append(record)
    refs = defaultdict(list)
    alignment = None
    video_timing = None
    if video is not None:
        video_header, frames, alignment = validate(video, Path(str(video) + '.alignment.jsonl'), path)
        video_timing = {key: video_header[key] for key in ('fps', 'time_base_num', 'time_base_den', 'width', 'height')}
        for frame in frames:
            camera = frame['camera_interpolation'][0]
            if camera is not None:
                refs[camera['target_tick']].append(dict(frame_index=frame['frame_index'],
                    pts=frame['pts'], timestamp_seconds=frame['pts'] * video_header['time_base_num'] / video_header['time_base_den'],
                    playback_tick=frame['demo_tick'], camera_timing=camera))
    last = {}
    rows = []
    for tick in sorted(set(range(header['playback_ticks'] + 1)) | set(by_tick)):
        messages = by_tick[tick]
        commands = []
        for message in messages:
            if message['kind'] != 5:
                continue
            slot, cmd = message['slot'], message['usercmd']
            candidate = action_candidate(cmd, last.get(slot), scale)
            commands.append(dict(slot=slot, byte_offset=message['byte_offset'],
                                 command_index=message['command_index'], raw=cmd,
                                 client_observations=capture_index.get((slot,message['command_index']), []),
                                 gym_candidate=candidate))
            last[slot] = cmd
        cameras = [dict(byte_offset=m['byte_offset'], slot=m['slot'], views=m['camera'])
                   for m in messages if m['kind'] == 2]
        rows.append(dict(type='tick', tick=tick, observations=dict(
            state=[None] * 18, state_valid=[False] * 18,
            packet_cameras=cameras, screen=dict(video=str(video.resolve()) if video else None,
                                               frames=refs.get(tick, []),
                                               tick_kind='camera_interpolation_target')),
            actions=commands, console_commands=[m['console_command'] for m in messages if m['kind'] == 4],
            episode=dict(is_first=tick == 0, is_last=tick == header['playback_ticks'],
                         terminated=None, truncated=None),
            quality=dict(training_ready=False,
                         reasons=['entity_state_not_decoded', 'pre_action_alignment_not_verified'] +
                         ([] if commands else ['no_recorded_usercmd']) +
                         ([] if refs.get(tick) else ['no_exact_camera_frame']))))
        matched=[s for c in commands if c['slot']==0 for s in c['client_observations']]
        if len(matched)==1:
            rows[-1]['observations']['state']=matched[0]['state']
            rows[-1]['observations']['state_valid']=matched[0]['state_valid']
            rows[-1]['observations']['state_stage']='before_decode_usercmd'
            rows[-1]['observations']['state_native_index']=matched[0]['native_index']
            if all(matched[0]['state_valid']):
                rows[-1]['quality']['reasons'].remove('entity_state_not_decoded')
    manifest = dict(type='header', schema='portal2-demonstration-v1', **header,
        action_fields=ACTION_FIELDS, state_fields=STATE_FIELDS, movement_scale=scale,
        movement_scale_source='explicit CLI parameter; not inferred from demo',
        tick_kind='demo_message_record_tick', state_source='replayed client entities before usercmd decode' if capture else 'unavailable; camera origin is not player origin',
        action_alignment='same record tick only; pre-action state correspondence unverified',
        episode_boundary_source='demo header; stop is not evidence of map success',
        split_group=header['player'], split_warning='group by runner and near-duplicate run before train/test splitting',
        alignment_validation=alignment, video_timing=video_timing, exporter_sha256=digest(Path(__file__)),
        client_capture=capture_report,
        training_ready=False)
    output.parent.mkdir(parents=True, exist_ok=True)
    partial = output.with_suffix(output.suffix + '.partial')
    with partial.open('w') as target:
        for row in [manifest, *rows, dict(type='end', complete=True, tick_records=len(rows))]:
            target.write(json.dumps(row, allow_nan=False, separators=(',', ':')) + '\n')
    partial.replace(output)
    return dict(demo=path.name, output=str(output.resolve()), ticks=len(rows),
                usercmds=sum(len(r['actions']) for r in rows),
                frame_references=sum(len(r['observations']['screen']['frames']) for r in rows),
                video_validated=alignment is not None, training_ready=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path, help='One demo or a directory (nonrecursive)')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--video-suffix', help='For example .bc-v1.mp4; requires a validated sidecar')
    parser.add_argument('--video-directory', type=Path, help='Directory of normalized videos (uses --video-suffix)')
    parser.add_argument('--capture-directory', type=Path, help='Native journals named <demo>.capture.jsonl')
    parser.add_argument('--movement-scale', type=float, default=175.0)
    args = parser.parse_args()
    require(math.isfinite(args.movement_scale) and args.movement_scale > 0, 'Invalid movement scale')
    paths = sorted(args.input.glob('*.dem')) if args.input.is_dir() else [args.input]
    require(bool(paths), 'No demo files found')
    results, errors = [], []
    for path in paths:
        try:
            video = path.with_suffix(args.video_suffix) if args.video_suffix else None
            if video is not None and args.video_directory:
                video = args.video_directory / video.name
            capture = args.capture_directory / (path.stem + '.capture.jsonl') if args.capture_directory else None
            result = export(path, args.output / (path.stem + '.jsonl'), video, args.movement_scale, capture)
            results.append(result)
            print(json.dumps(result), flush=True)
        except (OSError, ValueError, KeyError, TypeError, struct.error) as exc:
            errors.append(dict(demo=str(path), error=str(exc)))
            print(json.dumps(errors[-1]), file=sys.stderr, flush=True)
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'manifest.json').write_text(json.dumps(dict(schema='portal2-demonstration-batch-v1',
        results=results, errors=errors, training_ready=False), indent=2) + '\n')
    return bool(errors)


if __name__ == '__main__':
    raise SystemExit(main())
