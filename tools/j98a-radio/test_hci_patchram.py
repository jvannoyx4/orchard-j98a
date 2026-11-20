"""Exact USI Patchram diagnostic tests over Unix sockets, never Bluetooth."""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import unittest

HERE = Path(__file__).resolve().parent
EXE = Path(os.environ.get('J98A_PROBE_BIN_DIR', HERE)) / 'hci-patchram-test'
FIRMWARE = Path(os.environ['J98A_USI_FIRMWARE']) if os.environ.get('J98A_USI_FIRMWARE') else None
RESET = bytes.fromhex('01030c00')
VERSION = bytes.fromhex('01011000')
ROM = bytes.fromhex('00070010070f000766')


def complete(opcode, data=b'\0', credit=1):
    return bytes([4, 14, len(data)+3, credit]) + struct.pack('<H', opcode) + data


def firmware_records():
    data = FIRMWARE.read_bytes()
    result = []
    pos = 0
    while pos < len(data):
        length = data[pos+2]
        result.append(b'\1' + data[pos:pos+length+3])
        pos += length+3
    return result


@unittest.skipUnless(FIRMWARE is not None and FIRMWARE.is_file(), 'Set J98A_USI_FIRMWARE to the exact private/local USI HCD file')
class PatchramTests(unittest.TestCase):
    def probe(self, responder, success=False):
        a, b = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        a.settimeout(4)
        p = subprocess.Popen([str(EXE), str(b.fileno()), str(FIRMWARE)],
                             pass_fds=[b.fileno()], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True)
        b.close()
        try:
            responder(a)
            out, error = p.communicate(timeout=5)
            self.assertEqual(p.returncode == 0, success, (out, error))
            self.assertEqual(a.recv(512), b'', 'unexpected extra command')
            return out, error
        finally:
            if p.poll() is None:
                p.kill()
                p.wait()
            a.close()

    def prefix(self, sock, identity=ROM):
        self.assertEqual(sock.recv(512), RESET)
        sock.send(complete(0x0c03))
        self.assertEqual(sock.recv(512), VERSION)
        sock.send(complete(0x1001, identity))
        self.assertEqual(sock.recv(512), VERSION)
        sock.send(complete(0x1001, identity))

    def records(self, sock):
        self.prefix(sock)
        self.assertEqual(sock.recv(512), bytes.fromhex('012efc00'))
        sock.send(complete(0xfc2e))
        started = time.monotonic()
        for i, record in enumerate(firmware_records()):
            self.assertEqual(sock.recv(512), record)
            if i == 0:
                self.assertGreaterEqual(time.monotonic()-started, 0.045)
            sock.send(complete(struct.unpack_from('<H', record, 1)[0]))
        launched = time.monotonic()
        self.assertEqual(sock.recv(512), RESET)
        self.assertGreaterEqual(time.monotonic()-launched, 0.23)
        sock.send(complete(0x0c03))
        self.assertEqual(sock.recv(512), VERSION)

    def test_all_records_and_post_reset(self):
        def respond(sock):
            self.records(sock)
            patched = bytearray(ROM)
            patched[2:4] = struct.pack('<H', 0x1234)
            sock.send(complete(0x1001, patched))
        output, _ = self.probe(respond, True)
        parsed = [json.loads(x) for x in output.splitlines()]
        self.assertIn({'patchram_records_acknowledged': 295}, parsed)
        self.assertEqual(parsed[-1]['hci_revision'], 0x1234)

    def test_no_patch_over_different_revision(self):
        identity = bytearray(ROM)
        identity[2:4] = struct.pack('<H', 0x1001)
        self.probe(lambda sock: self.prefix(sock, identity))

    def test_minidrv_error_stops(self):
        def respond(sock):
            self.prefix(sock)
            self.assertEqual(sock.recv(512), bytes.fromhex('012efc00'))
            sock.send(complete(0xfc2e, b'\1'))
        self.probe(respond)

    def test_no_credit_stops_before_download(self):
        def respond(sock):
            self.prefix(sock)
            self.assertEqual(sock.recv(512), bytes.fromhex('012efc00'))
            sock.send(complete(0xfc2e, credit=0))
        self.probe(respond)

    def test_record_error_no_retry_or_launch(self):
        def respond(sock):
            self.prefix(sock)
            self.assertEqual(sock.recv(512), bytes.fromhex('012efc00'))
            sock.send(complete(0xfc2e))
            for i, record in enumerate(firmware_records()[:18]):
                self.assertEqual(sock.recv(512), record)
                sock.send(complete(0xfc4c, b'\1' if i == 17 else b'\0'))
        self.probe(respond)

    def test_post_patch_identity_error(self):
        def respond(sock):
            self.records(sock)
            identity = bytearray(ROM)
            identity[7:9] = struct.pack('<H', 0x6606)
            sock.send(complete(0x1001, identity))
        self.probe(respond)

    def test_invalid_firmware_rejected_before_transport(self):
        original = FIRMWARE.read_bytes()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'test.hcd'
            for data in (original[:-1], original+b'\0', bytes([original[0]^1])+original[1:]):
                path.write_bytes(data)
                p = subprocess.run([str(EXE), '-1', str(path)], capture_output=True,
                                   text=True, timeout=2)
                self.assertEqual(p.returncode, 2)
                self.assertNotIn('HCI Reset', p.stderr)
                self.assertIn('no device opened', p.stderr)


if __name__ == '__main__':
    unittest.main()
