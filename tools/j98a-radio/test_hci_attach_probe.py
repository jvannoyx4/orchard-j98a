"""Datagram HCI parser tests; only local Unix socketpairs, never hardware."""
import json
import os
import socket
import subprocess
import time
import unittest
from pathlib import Path

BINARY = Path(os.environ.get('J98A_PROBE_BIN_DIR', Path(__file__).parent)) / 'hci-attach-test'
RESET = bytes.fromhex('01030c00')
VERSION = bytes.fromhex('01011000')
RESET_OK = bytes.fromhex('040e0401030c00')
VERSION_OK = bytes.fromhex('040e0c01011000070010070f000766')


class ProbeTests(unittest.TestCase):
    def probe(self, responder, success=False):
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        parent.settimeout(4)
        proc = subprocess.Popen([str(BINARY), str(child.fileno())],
                                pass_fds=[child.fileno()], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True)
        child.close()
        try:
            self.assertEqual(parent.recv(256), RESET)
            responder(parent)
            output, error = proc.communicate(timeout=5)
            self.assertEqual(proc.returncode == 0, success, (output, error))
            self.assertEqual(parent.recv(256), b'', 'unexpected extra command')
            return output, error
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            parent.close()

    def test_success_and_reset_settling(self):
        def respond(sock):
            sock.send(RESET_OK)
            start = time.monotonic()
            self.assertEqual(sock.recv(256), VERSION)
            self.assertGreaterEqual(time.monotonic() - start, 0.09)
            sock.send(VERSION_OK)
        output, _ = self.probe(respond, True)
        data = [json.loads(line) for line in output.splitlines()]
        self.assertEqual(data[-1]['manufacturer'], 15)
        self.assertEqual(data[-1]['lmp_subversion'], 0x6607)

    def test_reset_error_no_version(self):
        self.probe(lambda s: s.send(bytes.fromhex('040e0401030c01')))

    def test_no_credit_no_version(self):
        self.probe(lambda s: s.send(bytes.fromhex('040e0400030c00')))

    def test_truncated_datagram(self):
        self.probe(lambda s: s.send(RESET_OK[:-1]))

    def test_packet_not_event(self):
        self.probe(lambda s: s.send(bytes.fromhex('020e0401030c00')))

    def test_hardware_error(self):
        self.probe(lambda s: s.send(bytes.fromhex('04100101')))

    def test_event_limit(self):
        self.probe(lambda s: [s.send(bytes.fromhex('04ff00')) for _ in range(16)])

    def test_other_chip_rejected(self):
        def respond(sock):
            sock.send(RESET_OK)
            self.assertEqual(sock.recv(256), VERSION)
            sock.send(VERSION_OK[:-2] + bytes.fromhex('0666'))
        self.probe(respond)

    def test_missing_reply_timeout(self):
        self.probe(lambda s: None)


if __name__ == '__main__':
    unittest.main()
