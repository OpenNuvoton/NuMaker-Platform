import json
from pathlib import Path
import tempfile
import unittest

from tools.pb5_relay import (
    SecurityLight, SensorFeed, ambient_lux, apply_dimmer, apply_relay,
    desired_state, dimmer_step, dimmer_target,
)


class ThresholdTests(unittest.TestCase):
    def test_pir_thresholds_keep_hysteresis(self):
        values = (0, 99, 100, 1800, 1801, 4095)
        self.assertEqual([desired_state(v, False) for v in values],
                         [False, False, False, False, True, True])
        self.assertEqual([desired_state(v, True) for v in values],
                         [False, False, True, True, True, True])

    def test_pir_motion_retriggers_minimum_on_time(self):
        light = SecurityLight()
        self.assertTrue(light.update(1801, False, 10))
        light.confirmed_on(10.3)
        self.assertTrue(light.update(0, True, 40.2))
        self.assertTrue(light.update(1941, True, 40.25))
        self.assertTrue(light.update(None, True, 70.24))
        self.assertFalse(light.update(99, True, 70.25))

    def test_dimmer_is_inverse_with_full_output_in_darkness(self):
        self.assertEqual([dimmer_target(lux) for lux in (0, 200, 400, 590, 7000)],
                         [100, 67, 50, 40, 5])

    def test_dimmer_ramps_to_target_in_both_directions(self):
        for start, lux in ((5, 0), (100, 7000)):
            with self.subTest(start=start, lux=lux):
                target = dimmer_target(lux)
                level = start
                self.assertNotEqual(dimmer_step(lux, level), target)
                for _ in range(20):
                    next_level = dimmer_step(lux, level)
                    self.assertTrue(min(level, target) <= next_level <= max(level, target))
                    level = next_level
                self.assertEqual(level, target)

    def test_valid_ambient_reading_does_not_require_motion_data(self):
        sample = {
            "updated_utc": "2020-01-01T00:00:02.1000000Z",
            "ambient": {"lux": 0, "valid": True, "saturated": False,
                        "updated_utc": "2020-01-01T00:00:02.0000000Z"},
        }
        self.assertEqual(ambient_lux(sample), 0)
        self.assertEqual(dimmer_target(ambient_lux(sample)), 100)

    def test_invalid_ambient_readings_are_rejected(self):
        stamp = "2020-01-01T00:00:02Z"
        reading = {"lux": 0, "valid": True, "saturated": False, "updated_utc": stamp}
        for change in ({"valid": False}, {"saturated": True}, {"lux": None},
                       {"lux": True}, {"lux": -1}, {"lux": float("nan")},
                       {"updated_utc": "2020-01-01T00:00:00Z"}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                ambient_lux({"updated_utc": stamp, "ambient": {**reading, **change}})
        with self.assertRaises(ValueError):
            ambient_lux({"updated_utc": stamp, "ambient": reading, "ambient_error": "I2C error"})

    def test_source_must_advance_then_expire_even_with_wrong_wall_clock(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'sample.json'
            feed=SensorFeed(path)
            path.write_text(json.dumps({'status':'online','updated_utc':'old-clock-1','analog':{'PB5':1939}}))
            with self.assertRaises(ValueError): feed.read(10)
            path.write_text(json.dumps({'status':'online','updated_utc':'old-clock-2','analog':{'PB5':1939}}))
            self.assertEqual(feed.read(10.1)["updated_utc"], "old-clock-2")
            self.assertEqual(feed.read(10.5)["updated_utc"], "old-clock-2")
            with self.assertRaises(ValueError): feed.read(11.2)

    def test_both_firmware_label_polarities_use_confirmed_physical_low(self):
        class FakeBoard:
            def __init__(self, active):
                self.active=active; self.level=1; self.commands=[]
            def send(self, commands, timeout_ms=None):
                self.commands.extend(commands)
                for cmd in commands:
                    if cmd=='RELAY ON': self.level=self.active
                    elif cmd=='RELAY OFF': self.level=1-self.active
                return {'lamp':{'brightness':90},'relay':{'pin':'PC2','pin_level':self.level,'active_level':self.active}}
        for active in (0,1):
            board=FakeBoard(active)
            self.assertEqual(apply_relay(board, True, 0)['relay']['pin_level'],0)
            self.assertEqual(apply_relay(board, False, 0)['relay']['pin_level'],1)
            self.assertFalse(any(cmd.startswith('LAMP ') for cmd in board.commands))

    def test_dimmer_write_checks_pwm_and_preserves_relay(self):
        class FakeBoard:
            def send(self, commands, timeout_ms=None):
                if commands != ["LAMP 40"]:
                    raise AssertionError(commands)
                return {"lamp": {"brightness": 40}, "relay": {"pin_level": 0}}

        self.assertEqual(apply_dimmer(FakeBoard(), 40, 0)["lamp"]["brightness"], 40)
        with self.assertRaises(RuntimeError):
            apply_dimmer(FakeBoard(), 40, 1)


if __name__=='__main__': unittest.main()
