"""Package the reviewed, unchanged four catalogues for the native engine."""
from pathlib import Path
import hashlib
import json
import shutil

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'smart-ground/public/data'
DESTINATION = ROOT / 'vSMR/data/SmartGround'
FILES = {
    'lfpg.json': 'lfpg.json',
    'lfpg-operations.json': 'lfpg_operations.json',
    'lfpg-corridors.json': 'lfpg_corridors.json',
    'lfpg-aircraft-restrictions.json': 'lfpg_aircraft_restrictions.json',
}


def main():
    DESTINATION.mkdir(parents=True, exist_ok=True)
    manifest = {'airport': 'LFPG', 'coordinateOrigin': [49.0097, 2.5479],
                'worksRestrictionsEnabled': False, 'files': []}
    for source_name, destination_name in FILES.items():
        source, destination = SOURCE / source_name, DESTINATION / destination_name
        data = source.read_bytes()
        shutil.copyfile(source, destination)
        assert destination.read_bytes() == data
        manifest['files'].append({'file': destination_name, 'source': f'smart-ground/public/data/{source_name}',
                                  'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
    (DESTINATION / 'source_manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
