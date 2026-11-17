from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from replay_format import ReplayClock, decode_pbh, parse_position, parse_flight_plan, surface_candidate, project


class ReplayFormatTests(unittest.TestCase):
    def test_clock_midnight_and_incoming_direction(self):
        clock = ReplayClock()
        self.assertTrue(clock.header('[23:59:58 >>>> LFPG_GND]'))
        self.assertEqual(clock.now, 0)
        self.assertTrue(clock.incoming)
        clock.header('[00:00:03 >>>> LFPG_GND]')
        self.assertEqual(clock.now, 5)
        self.assertEqual(clock.rollovers, 1)
        clock.header('[00:00:04 <<<2 LFPG_GND]')
        self.assertFalse(clock.incoming)
        self.assertEqual(clock.now, 6)

    def test_invalid_header_cannot_reuse_previous_report_time(self):
        clock = ReplayClock()
        clock.header('[12:00:00 >>>> LFPG_GND]')
        self.assertFalse(clock.header('[25:00:00 >>>> LFPG_GND]'))
        self.assertIsNone(clock.now)
        self.assertFalse(clock.incoming)

    def test_unsigned_pbh_bit_layout_and_north_zero(self):
        self.assertEqual(decode_pbh(0), (0, False))
        self.assertEqual(decode_pbh(2), (0, True))
        self.assertEqual(decode_pbh((256 << 2) | 2), (90, True))
        self.assertEqual(decode_pbh(0xFFFFFFFF), (1023 * 360 / 1024, True))
        with self.assertRaises(ValueError):
            decode_pbh(-1)
        with self.assertRaises(ValueError):
            decode_pbh(0x100000000)

    def test_position_fields_preserve_altitude_units_and_pressure_delta(self):
        report = parse_position('@N:TEST123:1000:1:49.00970:2.54790:392:12:1026:-25')
        self.assertIsNotNone(report)
        self.assertEqual(report['altitudeFeet'], 392)
        self.assertEqual(report['pressureAltitudeFeet'], 367)
        self.assertEqual(report['groundSpeedKnots'], 12)
        self.assertEqual(report['headingDegrees'], 90)
        self.assertTrue(report['reportedOnGround'])
        self.assertEqual(project(report['latitude'], report['longitude']), (0, 0))

    def test_invalid_positions_and_nontelemetry_are_not_exported(self):
        self.assertIsNone(parse_position('@N:TEST123:1000:1:NaN:2.5:392:12:0:0'))
        self.assertIsNone(parse_position('@N:TEST123:1000:1:49:181:392:12:0:0'))
        self.assertIsNone(parse_position('@N:TEST123:1000:1:49:2:392:-1:0:0'))
        self.assertIsNone(parse_position('#AAUSER:SERVER:PRIVATE'))
        self.assertIsNone(parse_position('#TMUSER:TARGET:CHAT'))

    def test_flightplan_uses_only_type_and_endpoints(self):
        identity, plan = parse_flight_plan('$FPTEST123:*A:I:A388/J-SDFG/LB1:480:LFPG:1200:1200:39000:OMDB:0:0:0:0:PRIVATE REMARKS')
        self.assertEqual(identity, 'TEST123')
        self.assertEqual(plan, {'typeCode': 'A388', 'origin': 'LFPG', 'destination': 'OMDB'})
        self.assertNotIn('PRIVATE', str(plan))

    def test_surface_extraction_is_only_a_broad_filter(self):
        report = parse_position('@N:TEST123:1000:1:49.00970:2.54790:392:12:1024:0')
        self.assertTrue(surface_candidate(report), 'Missing groundbit does not prove airborne.')
        report['altitudeFeet'] = 1500
        self.assertFalse(surface_candidate(report))
        report['altitudeFeet'] = 392
        report['groundSpeedKnots'] = 180
        self.assertFalse(surface_candidate(report))


if __name__ == '__main__':
    unittest.main()
