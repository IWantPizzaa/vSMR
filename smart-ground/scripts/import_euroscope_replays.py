"""Stream local replays to an anonymous, telemetry-only offline validation cache.

Original files are opened rb only. Unrelated PDUs (authentication, names, chat)
are never serialized or printed. Each source is SHA-256 hashed as it is scanned.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import time
from replay_format import ReplayClock, parse_position, parse_flight_plan, surface_candidate, in_lfpg_region, project

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = Path(r'C:\Users\mathi\AppData\Roaming\EuroScope\aa-REPLAYS')


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, default=DEFAULT_SOURCE)
    parser.add_argument('--output', type=Path, default=ROOT / 'tmp/replay-cache')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    files = sorted(path for path in args.source.iterdir() if path.suffix.lower() == '.txt' and path.stat().st_size > 1000)
    inventory = []
    code_files = [Path(__file__), Path(__file__).with_name('replay_format.py')]
    code_hashes = {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest() for path in code_files}
    started = time.monotonic()
    for index, source in enumerate(files):
        stat_before = source.stat()
        digest = hashlib.sha256()
        clock = ReplayClock()
        identities: dict[str, str] = {}
        generations: dict[str, int] = {}
        previous: dict[str, tuple[int, tuple]] = {}
        plans: dict[str, dict] = {}
        target_plans: dict[str, dict] = {}
        counts = {'positionPackets': 0, 'lfpgRegionPackets': 0, 'surfaceCandidates': 0, 'duplicateTimestampPackets': 0,
                  'conflictingSameSecondPackets': 0, 'outOfOrderPackets': 0, 'malformedPositionPackets': 0,
                  'flightPlans': 0, 'reportedGroundFlagCandidates': 0, 'reconnectGenerations': 0}
        cache_name = f'replay-{index + 1:02d}.jsonl'
        with source.open('rb') as original, (args.output / cache_name).open('w', encoding='utf-8', newline='\n') as output:
            for line_number, raw in enumerate(original, 1):
                digest.update(raw)
                if raw[:1] not in (b'[', b'@', b'$', b'#'):
                    continue
                line = raw.decode('utf-8', errors='replace').strip()
                if line.startswith('['):
                    clock.header(line)
                    continue
                if not clock.incoming or clock.now is None:
                    continue
                if line.startswith('$FP'):
                    plan = parse_flight_plan(line)
                    if plan:
                        plans[plan[0]] = plan[1]
                        counts['flightPlans'] += 1
                    continue
                if line.startswith('#DP'):
                    identity = line[3:].split(':', 1)[0]
                    if identity in identities:
                        del identities[identity]
                    previous.pop(identity, None)
                    continue
                if not line.startswith('@'):
                    continue
                counts['positionPackets'] += 1
                report = parse_position(line)
                if not report:
                    counts['malformedPositionPackets'] += 1
                    continue
                if in_lfpg_region(report):
                    counts['lfpgRegionPackets'] += 1
                if not surface_candidate(report):
                    continue
                identity = report.pop('identity')
                fingerprint = tuple(report.values())
                last = previous.get(identity)
                if last and clock.now <= last[0]:
                    if clock.now < last[0]:
                        counts['outOfOrderPackets'] += 1
                    elif fingerprint == last[1]:
                        counts['duplicateTimestampPackets'] += 1
                    else:
                        counts['conflictingSameSecondPackets'] += 1
                    continue
                if last and clock.now - last[0] > 120:
                    identities.pop(identity, None)
                if identity not in identities:
                    generations[identity] = generations.get(identity, 0) + 1
                    if generations[identity] > 1:
                        counts['reconnectGenerations'] += 1
                    identities[identity] = f'R{index + 1:02d}-T{len(target_plans) + 1:04d}'
                    target_plans[identities[identity]] = {'typeCode': 'UNKNOWN', 'origin': None, 'destination': None}
                target = identities[identity]
                if identity in plans:
                    target_plans[target] = plans[identity]
                previous[identity] = (clock.now, fingerprint)
                x, y = project(report['latitude'], report['longitude'])
                record = {'aircraftId': target, 'observedAt': clock.now, 'sourceLine': line_number,
                          'x': round(x, 4), 'y': round(y, 4), **report}
                output.write(json.dumps(record, separators=(',', ':')) + '\n')
                counts['surfaceCandidates'] += 1
                counts['reportedGroundFlagCandidates'] += int(report['reportedOnGround'])
        stat_after = source.stat()
        if stat_before.st_size != stat_after.st_size or stat_before.st_mtime_ns != stat_after.st_mtime_ns:
            raise RuntimeError(f'Replay changed during scan: {source.name}')
        metadata = {'fileName': source.name, 'bytes': stat_before.st_size, 'sha256': digest.hexdigest(),
                    'lastWriteTimeNs': str(stat_before.st_mtime_ns), 'cache': cache_name, 'counts': counts,
                    'targets': target_plans, 'durationSeconds': clock.now or 0, 'midnightRollovers': clock.rollovers}
        (args.output / f'replay-{index + 1:02d}.metadata.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
        inventory.append({key: value for key, value in metadata.items() if key != 'targets'})
        print(f'{index + 1}/{len(files)} {source.name}: {counts["surfaceCandidates"]} LFPG surface candidates, {len(target_plans)} anonymous tracks.', flush=True)
    assert code_hashes == {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest() for path in code_files}, 'Parser changed during import.'
    summary = {'sourceDirectoryName': args.source.name, 'files': inventory, 'elapsedSeconds': round(time.monotonic() - started, 2), 'codeSha256': code_hashes,
               'format': 'euroscope-incoming-fsd-telemetry-v1', 'clock': 'logged receipt time, integer seconds, midnight unwrapped',
               'identities': 'Per-file anonymous track generations; no names, authentication PDUs or chat exported.'}
    (args.output / 'inventory.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
