"""Managed-controller tests using synthetic private addresses and Unix sockets."""
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import tempfile
import unittest

EXE = Path(os.environ.get('J98A_PROBE_BIN_DIR', Path(__file__).parent)) / 'hci-managed-test'
FACTORY = bytes.fromhex('102030405060')  # Synthetic fixture, not a device identity.
READ_INFO = bytes.fromhex('040007000000')
POWER_ON = bytes.fromhex('05000700010001')
POWER_OFF = bytes.fromhex('05000700010000')
OFF = 0x280
ON = OFF | 1


def complete(opcode, payload=b'', status=0, index=7):
    data = struct.pack('<HB', opcode, status) + payload
    return struct.pack('<HHH', 1, index, len(data)) + data


def command_status(opcode, status, index=7):
    return struct.pack('<HHHHB', 2, index, 3, opcode, status)


def info(address=FACTORY[::-1], version=8, maker=15, settings=OFF):
    return (address + struct.pack('<BHII', version, maker, 0x2ff, settings) +
            bytes(3) + b'PRIVATE-test'.ljust(249, b'\0') + bytes(11))


class ManagedTests(unittest.TestCase):
    def probe(self, respond, success=False, hold=False, first_send_result=None):
        a, b = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        a.settimeout(4)
        with tempfile.TemporaryDirectory() as directory:
            factory = Path(directory)/'factory.bin'
            factory.write_bytes(FACTORY)
            cmd = [str(EXE), str(b.fileno()), str(factory)] + (['hold'] if hold else [])
            env = os.environ.copy()
            if first_send_result is not None:
                env['HCI_MANAGED_TEST_FIRST_SEND_RESULT'] = str(first_send_result)
            proc = subprocess.Popen(cmd, pass_fds=[b.fileno()], stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True, env=env)
            b.close()
            try:
                self.assertEqual(a.recv(4096), READ_INFO)
                respond(a, proc)
                output, error = proc.communicate(timeout=5)
                self.assertEqual(proc.returncode == 0, success, (output, error))
                self.assertEqual(a.recv(4096), b'', 'unexpected extra command')
                self.assertNotIn('PRIVATE-test', output + error)
                self.assertNotIn(FACTORY.hex(), output + error)
                self.assertNotIn(':'.join(f'{x:02x}' for x in FACTORY), output + error)
                return output, error
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()
                a.close()

    def power_cycle(self, sock):
        self.assertEqual(sock.recv(4096), POWER_ON)
        sock.send(complete(5, struct.pack('<I', ON)))
        self.assertEqual(sock.recv(4096), POWER_OFF)
        sock.send(complete(5, struct.pack('<I', OFF)))

    def test_success_after_setup_wait(self):
        def respond(sock, proc):
            sock.send(complete(4, status=0x11))
            self.assertEqual(sock.recv(4096), READ_INFO)
            sock.send(complete(4, info(), index=8))
            sock.send(complete(4, info()))
            self.power_cycle(sock)
        out, _ = self.probe(respond, True)
        self.assertTrue(json.loads(out.splitlines()[-1])['managed_ready'])

    def test_zero_send_with_setup_status_waits_for_initialization(self):
        def respond(sock, proc):
            sock.send(command_status(4, 0x11))
            self.assertEqual(sock.recv(4096), READ_INFO)
            sock.send(complete(4, info()))
            self.power_cycle(sock)
        out, _ = self.probe(respond, True, first_send_result=0)
        self.assertTrue(json.loads(out.splitlines()[-1])['managed_ready'])

    def test_zero_send_with_real_rejection_is_reported(self):
        _, err = self.probe(lambda s, p: s.send(command_status(4, 0x0c)),
                            first_send_result=0)
        self.assertIn('status 12, errno 0', err)

    def test_short_positive_send_is_not_retried(self):
        _, err = self.probe(lambda s, p: None, first_send_result=1)
        self.assertIn('status -1, errno 5', err)

    def test_address_mismatch_never_powered(self):
        self.probe(lambda s, p: s.send(complete(4, info(address=bytes(6)))))

    def test_bad_address_autopowered_is_powered_down(self):
        def respond(sock, proc):
            sock.send(complete(4, info(address=bytes(6), settings=ON)))
            self.assertEqual(sock.recv(4096), POWER_OFF)
            sock.send(complete(5, struct.pack('<I', OFF)))
        self.probe(respond)

    def test_wrong_version_stops(self):
        self.probe(lambda s, p: s.send(complete(4, info(version=7))))

    def test_wrong_manufacturer_stops(self):
        self.probe(lambda s, p: s.send(complete(4, info(maker=2))))

    def test_unexpected_discoverable_stops(self):
        self.probe(lambda s, p: s.send(complete(4, info(settings=OFF | 8))))

    def test_short_info_rejected(self):
        self.probe(lambda s, p: s.send(complete(4, info()[:-1])))

    def test_oversized_datagram_rejected(self):
        self.probe(lambda s, p: s.send(complete(4, bytes(5000))))

    def test_power_on_failure_attempts_power_off(self):
        def respond(sock, proc):
            sock.send(complete(4, info()))
            self.assertEqual(sock.recv(4096), POWER_ON)
            sock.send(complete(5, status=3))
            self.assertEqual(sock.recv(4096), POWER_OFF)
            sock.send(complete(5, struct.pack('<I', OFF)))
        self.probe(respond)

    def test_power_down_failure_reported(self):
        def respond(sock, proc):
            sock.send(complete(4, info()))
            self.assertEqual(sock.recv(4096), POWER_ON)
            sock.send(complete(5, struct.pack('<I', ON)))
            self.assertEqual(sock.recv(4096), POWER_OFF)
            sock.send(complete(5, status=3))
        self.probe(respond)

    def test_sigterm_cleans_up_held_adapter(self):
        def respond(sock, proc):
            sock.send(complete(4, info()))
            self.assertEqual(sock.recv(4096), POWER_ON)
            sock.send(complete(5, struct.pack('<I', ON)))
            self.assertIn('factory_address_match', proc.stdout.readline())
            self.assertIn('managed_ready', proc.stdout.readline())
            proc.send_signal(signal.SIGTERM)
            self.assertEqual(sock.recv(4096), POWER_OFF)
            sock.send(complete(5, struct.pack('<I', OFF)))
        self.probe(respond, True, True)


if __name__ == '__main__':
    unittest.main()
