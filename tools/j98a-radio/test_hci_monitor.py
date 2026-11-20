"""Synthetic datagrams test framing, status extraction and payload privacy."""
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import unittest

HERE = Path(__file__).resolve().parent
BINARY = Path(os.environ.get('J98A_PROBE_BIN_DIR', HERE))/'hci-monitor-test'


def packet(kind, body, index=7):
    return struct.pack('<HHH', kind, index, len(body))+body


class Monitor(unittest.TestCase):
    def run_packets(self, packets, *, malformed=False):
        host, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
        proc = subprocess.Popen([str(BINARY), str(child.fileno())], pass_fds=[child.fileno()],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        child.close()
        ready = proc.stdout.readline()
        self.assertTrue(json.loads(ready)['hci_metadata_monitor_ready'])
        for value in packets:
            host.send(value)
        if not malformed:
            # A final harmless visible event is a read-side synchronization barrier.
            host.send(packet(1, b''))
            lines = []
            while True:
                line = proc.stdout.readline()
                self.assertTrue(line)
                lines.append(line)
                row = json.loads(line)
                if row.get('monitor_opcode') == 1:
                    break
            proc.send_signal(signal.SIGTERM)
        else:
            lines = []
        out, err = proc.communicate(timeout=3)
        host.close()
        return proc.returncode, ready+''.join(lines)+out, err

    def test_reset_and_version_metadata(self):
        code, out, err = self.run_packets([
            packet(2, bytes.fromhex('030c00')),
            packet(3, bytes.fromhex('0e0401030c00')),
            packet(3, bytes.fromhex('0f0400010110')),
        ])
        self.assertEqual((code, err), (0, ''))
        rows = [json.loads(x) for x in out.splitlines()]
        self.assertEqual(rows[1]['opcode'], 0x0c03)
        self.assertEqual(rows[2]['command_credit'], 1)
        self.assertEqual(rows[2]['status'], 0)
        self.assertEqual(rows[3]['opcode'], 0x1001)

    def test_identity_payloads_are_never_printed(self):
        secret = b'PRIVATE-IDENTITY'
        code, out, err = self.run_packets([
            packet(0, secret), packet(10, secret), packet(12, secret),
            packet(17, secret), packet(4, secret),
            packet(2, struct.pack('<HB', 0x0c13, len(secret))+secret),
            packet(3, bytes([0x0e, len(secret)+3, 1, 9, 0x10])+secret),
            packet(3, bytes([0xff, len(secret)])+secret),
        ])
        self.assertEqual((code, err), (0, ''))
        self.assertNotIn(secret.decode(), out)
        self.assertNotIn(secret.hex(), out)
        rows = [json.loads(x) for x in out.splitlines()]
        cc = next(x for x in rows if x.get('kind') == 'command_complete')
        self.assertNotIn('status', cc)
        self.assertEqual(cc['opcode'], 0x1009)

    def test_malformed_hci_lengths_fail_without_payload(self):
        for body in (b'\x0e\x04\x01\x03\x0c', b'\x0f\x03\x00\x01\x03', b'\xff\x03AB'):
            with self.subTest(body=body):
                code, out, err = self.run_packets([packet(3, body)], malformed=True)
                self.assertEqual(code, 1)
                self.assertIn('payload omitted', err)
                self.assertNotIn(body.hex(), out)

    def test_bad_outer_length_fails(self):
        code, out, err = self.run_packets([struct.pack('<HHH', 3, 7, 500)+b'PRIVATE'], malformed=True)
        self.assertEqual(code, 1)
        self.assertNotIn('PRIVATE', out+err)


if __name__ == '__main__':
    unittest.main()
