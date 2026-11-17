"""Read only telemetry from EuroScope FSD text replays; never export other PDUs.

PBH layout/scale and @ field order verified against primary Swift source:
https://apidocs.swift-project.org/pbh_8h_source.html
https://apidocs.swift-project.org/pilotdataupdate_8cpp_source.html
"""
from __future__ import annotations

from dataclasses import dataclass
import math
import re

HEADER = re.compile(r'^\[(\d{2}):(\d{2}):(\d{2})\s+([<>]+\d*)\s+[^\]]+\]$')
IDENTITY = re.compile(r'^[A-Z0-9_-]{1,32}$')


@dataclass
class ReplayClock:
    day: int = 0
    previous_seconds: int | None = None
    first_seconds: int | None = None
    incoming: bool = False
    now: int | None = None
    rollovers: int = 0

    def header(self, text: str) -> bool:
        self.incoming = False
        self.now = None
        match = HEADER.fullmatch(text.strip())
        if not match:
            return False
        hour, minute, second = map(int, match.group(1, 2, 3))
        if hour > 23 or minute > 59 or second > 59:
            return False
        seconds = hour * 3600 + minute * 60 + second
        if self.previous_seconds is not None and seconds < self.previous_seconds - 12 * 3600:
            self.day += 86400
            self.rollovers += 1
        self.previous_seconds = seconds
        absolute = self.day + seconds
        if self.first_seconds is None:
            self.first_seconds = absolute
        self.now = absolute - self.first_seconds
        self.incoming = match.group(4).startswith('>')
        return True


def decode_pbh(value: str | int) -> tuple[float, bool]:
    packed = int(value)
    if packed < 0 or packed > 0xFFFFFFFF:
        raise ValueError('PBH outside unsigned 32-bit range')
    return ((packed >> 2) & 1023) * 360 / 1024, bool(packed & 2)


def parse_position(text: str) -> dict | None:
    fields = text.rstrip('\r\n').split(':')
    if len(fields) < 10 or not re.fullmatch(r'@[A-Z]', fields[0]) or not IDENTITY.fullmatch(fields[1]):
        return None
    try:
        latitude, longitude = float(fields[4]), float(fields[5])
        altitude, speed, delta = int(fields[6]), int(fields[7]), int(fields[9])
        heading, ground_flag = decode_pbh(fields[8])
    except (ValueError, OverflowError):
        return None
    if not math.isfinite(latitude) or not math.isfinite(longitude) or not -90 <= latitude <= 90 or not -180 <= longitude <= 180 or not 0 <= speed <= 2000 or not -2000 <= altitude <= 100000:
        return None
    return {'identity': fields[1], 'latitude': latitude, 'longitude': longitude,
            'altitudeFeet': altitude, 'pressureAltitudeFeet': altitude + delta,
            'groundSpeedKnots': speed, 'headingDegrees': heading,
            'reportedOnGround': ground_flag}


def parse_flight_plan(text: str) -> tuple[str, dict] | None:
    fields = text.rstrip('\r\n').split(':')
    if len(fields) < 10 or not fields[0].startswith('$FP') or not IDENTITY.fullmatch(fields[0][3:]):
        return None
    aircraft = fields[3].split('/')[0].split('-')[0].upper()
    return fields[0][3:], {
        'typeCode': aircraft if re.fullmatch(r'[A-Z0-9]{2,4}', aircraft) else 'UNKNOWN',
        'origin': fields[5] if re.fullmatch(r'[A-Z0-9]{4}', fields[5]) else None,
        'destination': fields[9] if re.fullmatch(r'[A-Z0-9]{4}', fields[9]) else None,
    }


def in_lfpg_region(report: dict) -> bool:
    return 48.975 <= report['latitude'] <= 49.045 and 2.445 <= report['longitude'] <= 2.665


def surface_candidate(report: dict) -> bool:
    # Broad extraction only: the validator adds taxiway distance and sustained
    # ground-altitude/speed context. The FSD ground bit is not trusted alone.
    return in_lfpg_region(report) and 0 <= report['altitudeFeet'] <= 700 and report['groundSpeedKnots'] <= 55


def project(latitude: float, longitude: float) -> tuple[float, float]:
    return ((longitude - 2.5479) * 111320 * math.cos(math.radians(49.0097)),
            (latitude - 49.0097) * 111320)
