"""Add Paris runway controls without changing SID routes or procedure data."""
import argparse
from datetime import datetime
import json
from pathlib import Path
import re
import shutil

AIRPORTS = ("lfpg", "lfpo", "lfpn", "lfpv", "lfpt", "lfob")


def configure(directory: Path) -> None:
    pending = []
    for airport in AIRPORTS:
        path = directory / f"{airport}.json"
        original = path.read_bytes()
        text = original.decode("utf-8-sig")
        data = json.loads(text)
        rules = data[airport.upper()]["customRules"]
        for name in list(rules):
            if name.lower() in ("paris_auto", "paris_manual_config"):
                del rules[name]
        for name, default in (("linked", True), ("unlinked", False)):
            rules.setdefault(name, default)
        if airport in ("lfpn", "lfpv", "lfpt", "lfob"):
            for name in ("wlpg", "elpg", "wipg", "eipg"):
                rules.setdefault(name, False)
        newline = "\r\n" if b"\r\n" in original else "\n"
        block = json.dumps(rules, indent=2).replace("\n", newline + "    ")
        updated, count = re.subn(r'("customRules"\s*:\s*)\{[^{}]*\}',
                                lambda match: match[1] + block, text, count=1)
        if count != 1 or json.loads(updated) != data:
            raise ValueError(f"Cannot safely replace customRules in {path}")
        encoded = updated.encode("utf-8")
        if original.startswith(b"\xef\xbb\xbf"):
            encoded = b"\xef\xbb\xbf" + encoded
        if encoded != original:
            pending.append((path, encoded))

    # vSID loads airport files from this setting, not from a fixed folder name.
    # Point the adjacent main config at the directory explicitly selected above.
    main_path = directory.parent / "vSIDConfig.json"
    if main_path.is_file():
        original = main_path.read_bytes()
        text = original.decode("utf-8-sig")
        data = json.loads(text)
        configured = data.get("airportConfigs")
        if not isinstance(configured, str):
            raise ValueError(f"Missing airportConfigs path in {main_path}")
        if (main_path.parent / configured).resolve() != directory.resolve():
            selected = directory.name + "/"
            data["airportConfigs"] = selected
            updated, count = re.subn(r'("airportConfigs"\s*:\s*)"(?:[^"\\]|\\.)*"',
                                    lambda match: match[1] + json.dumps(selected), text, count=1)
            if count != 1 or json.loads(updated) != data:
                raise ValueError(f"Cannot safely replace airportConfigs in {main_path}")
            encoded = updated.encode("utf-8")
            if original.startswith(b"\xef\xbb\xbf"):
                encoded = b"\xef\xbb\xbf" + encoded
            pending.append((main_path, encoded))
    if not pending:
        print("Paris configuration already installed.")
        return
    backup = directory.parent / "Backups" / ("paris-" + datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    backup.mkdir(parents=True)
    for path, _ in pending:
        shutil.copy2(path, backup / path.name)
    for path, encoded in pending:
        path.write_bytes(encoded)
        print(f"Updated {path.name}")
    print(f"Original configurations: {backup}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="vSID AirportsConfig directory")
    configure(parser.parse_args().directory)
