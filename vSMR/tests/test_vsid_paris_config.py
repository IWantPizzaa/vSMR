"""Regression checks for the external vSID configuration migration."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "configure_vsid_paris", Path(__file__).parents[1] / "tools/configure_vsid_paris.py")
config = importlib.util.module_from_spec(spec)
spec.loader.exec_module(config)


class ParisConfigurationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name) / "AirportsConfig"
        self.directory.mkdir()
        self.original = {}
        for airport in config.AIRPORTS:
            data = {airport.upper(): {"customRules": {"opposing": False},
                                     "sids": {"EXAMPLE": {"rwy": "27L", "initial": 5000}}}}
            original = (json.dumps(data, indent=2) + "\n").replace("\n", "\r\n").encode()
            self.original[airport] = original
            (self.directory / f"{airport}.json").write_bytes(original)

    def test_preserves_procedures_and_makes_exact_backups(self):
        config.configure(self.directory)
        backup = next((self.directory.parent / "Backups").iterdir())
        for airport, original in self.original.items():
            path = self.directory / f"{airport}.json"
            current = json.loads(path.read_bytes())[airport.upper()]
            expected = json.loads(original)[airport.upper()]
            self.assertEqual(current["sids"], expected["sids"])
            self.assertTrue(current["customRules"]["paris_auto"])
            self.assertTrue(current["customRules"]["linked"])
            self.assertFalse(current["customRules"]["unlinked"])
            self.assertEqual((backup / path.name).read_bytes(), original)
            self.assertNotIn(b"\n", path.read_bytes().replace(b"\r\n", b""))
            if airport in ("lfpn", "lfpv", "lfpt", "lfob"):
                self.assertTrue(all(name in current["customRules"] for name in ("wlpg", "elpg", "wipg", "eipg")))
        before = {path: path.read_bytes() for path in self.directory.iterdir()}
        config.configure(self.directory)
        self.assertEqual(before, {path: path.read_bytes() for path in self.directory.iterdir()})
        self.assertEqual(len(list(backup.parent.iterdir())), 1)

    def test_preflight_failure_does_not_change_other_airports(self):
        (self.directory / "lfob.json").write_text("invalid JSON")
        with self.assertRaises(json.JSONDecodeError):
            config.configure(self.directory)
        for airport in config.AIRPORTS[:-1]:
            self.assertEqual((self.directory / f"{airport}.json").read_bytes(), self.original[airport])

    def test_keeps_existing_manual_configuration(self):
        path = self.directory / "lfpg.json"
        data = json.loads(path.read_bytes())
        data["LFPG"]["customRules"].update(linked=False, unlinked=True, paris_auto=False)
        path.write_text(json.dumps(data))
        config.configure(self.directory)
        self.assertEqual(json.loads(path.read_bytes()), data)

    def test_corrects_main_config_folder_and_preserves_other_settings(self):
        main = self.directory.parent / "vSIDConfig.json"
        original = b'{\r\n  "airportConfigs": "vSID AirportConfigs/",\r\n  "setting": 42\r\n}\r\n'
        main.write_bytes(original)
        config.configure(self.directory)
        self.assertEqual(main.read_bytes(), original.replace(b"vSID AirportConfigs/", b"AirportsConfig/"))
        backup = next((self.directory.parent / "Backups").iterdir())
        self.assertEqual((backup / main.name).read_bytes(), original)
        selected = json.loads(main.read_bytes())["airportConfigs"]
        self.assertEqual((main.parent / selected).resolve(), self.directory.resolve())
        config.configure(self.directory)
        self.assertEqual(len(list(backup.parent.iterdir())), 1)

    def test_invalid_main_config_is_detected_before_any_changes(self):
        (self.directory.parent / "vSIDConfig.json").write_text("invalid JSON")
        with self.assertRaises(json.JSONDecodeError):
            config.configure(self.directory)
        for airport in config.AIRPORTS:
            self.assertEqual((self.directory / f"{airport}.json").read_bytes(), self.original[airport])

    def test_keeps_equivalent_existing_main_config_path(self):
        main = self.directory.parent / "vSIDConfig.json"
        original = json.dumps({"airportConfigs": str(self.directory.resolve()), "setting": 42}).encode()
        main.write_bytes(original)
        config.configure(self.directory)
        self.assertEqual(main.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
