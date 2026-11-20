"""Host PTY integration tests; never accesses hardware UARTs or GPIOs."""
import json
import os
from pathlib import Path
import pty
import select
import subprocess
import termios
import time
import unittest

PROGRAM = Path(os.environ.get('J98A_PROBE_BIN_DIR', Path(__file__).parent)) / 'h4-probe-host'
RESET = b"\x01\x03\x0c\x00"
VERSION = b"\x01\x01\x10\x00"
RESET_OK = b"\x04\x0e\x04\x01\x03\x0c\x00"
VERSION_OK = bytes.fromhex("04 0e 0c 01 01 10 00 08 34 12 08 0f 00 78 56")


def receive(fd, count):
    data = b""
    deadline = time.monotonic() + 2
    while len(data) < count:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([fd], [], [], remaining)[0]:
            raise AssertionError("helper did not send expected command")
        data += os.read(fd, count - len(data))
    return data


class ProbeTests(unittest.TestCase):
    def run_probe(self, responder, expected_success=False):
        master, slave = pty.openpty()
        original = termios.tcgetattr(slave)
        proc = subprocess.Popen([str(PROGRAM), os.ttyname(slave), "250"],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            self.assertEqual(receive(master, 4), RESET)
            responder(master)
            stdout, stderr = proc.communicate(timeout=2)
            self.assertEqual(proc.returncode == 0, expected_success,
                             (proc.returncode, stdout, stderr))
            self.assertEqual(termios.tcgetattr(slave), original)
            # There must be no unrequested retry or third command.
            self.assertFalse(select.select([master], [], [], 0.02)[0])
            return stdout.decode(), stderr.decode()
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            os.close(slave)
            os.close(master)

    def test_fragmented_success_ignores_unrelated_events(self):
        def reply(fd):
            # Vendor event and unrelated completion must not satisfy Reset.
            os.write(fd, bytes.fromhex("04 ff 02 aa bb 04 0e 04 01 02 10 00"))
            for byte in RESET_OK:
                os.write(fd, bytes([byte]))
                time.sleep(0.002)
            self.assertEqual(receive(fd, 4), VERSION)
            for byte in VERSION_OK:
                os.write(fd, bytes([byte]))
                time.sleep(0.002)
        stdout, _ = self.run_probe(reply, True)
        version = json.loads(stdout.splitlines()[-1])
        self.assertEqual(version, {
            "command": "read_local_version", "status": 0,
            "hci_version": 8, "hci_revision": 0x1234, "lmp_version": 8,
            "manufacturer": 15, "lmp_subversion": 0x5678,
        })

    def test_reset_failure_stops_before_version(self):
        _, err = self.run_probe(lambda fd: os.write(fd, RESET_OK[:-1] + b"\x01"))
        self.assertIn("completion status 0x01", err)

    def test_no_command_credit_stops(self):
        _, err = self.run_probe(lambda fd: os.write(fd, bytes.fromhex("04 0e 04 00 03 0c 00")))
        self.assertIn("no command credit", err)

    def test_command_status_rejection(self):
        _, err = self.run_probe(lambda fd: os.write(fd, bytes.fromhex("04 0f 04 01 01 03 0c")))
        self.assertIn("Command Status 0x01", err)

    def test_wrong_h4_type(self):
        _, err = self.run_probe(lambda fd: os.write(fd, b"\x02"))
        self.assertIn("Unexpected H4 packet type", err)

    def test_hardware_error(self):
        _, err = self.run_probe(lambda fd: os.write(fd, bytes.fromhex("04 10 01 03")))
        self.assertIn("Hardware Error", err)

    def test_no_response_times_out(self):
        _, err = self.run_probe(lambda fd: None)
        self.assertIn("timed out", err)

    def test_partial_version_times_out(self):
        def reply(fd):
            os.write(fd, RESET_OK)
            self.assertEqual(receive(fd, 4), VERSION)
            os.write(fd, VERSION_OK[:-1])
        _, err = self.run_probe(reply)
        self.assertIn("Read Local Version", err)
        self.assertIn("timed out", err)

    def test_wrong_version_length_rejected(self):
        def reply(fd):
            os.write(fd, RESET_OK)
            self.assertEqual(receive(fd, 4), VERSION)
            os.write(fd, bytes.fromhex("04 0e 04 01 01 10 00"))
        _, err = self.run_probe(reply)
        self.assertIn("reply length 4, expected 12", err)

    def test_unrelated_event_limit(self):
        self.run_probe(lambda fd: os.write(fd, b"\x04\xff\x00" * 16))


if __name__ == "__main__":
    unittest.main()
