"""Read-only adapter checks issue only ReadInfo and never print identities."""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
EXE = Path(os.environ.get('J98A_PROBE_BIN_DIR', HERE))/'hci-adapter-check-test'
FACTORY = bytes.fromhex('102030405060')


def reply(settings=0x281, address=FACTORY[::-1], version=8, maker=15, supported=0xa81):
    data = address+struct.pack('<BHII', version, maker, supported, settings)
    data += bytes(3)+b'PRIVATE-adapter-name'.ljust(249,b'\0')+bytes(11)
    return struct.pack('<HHHHB',1,7,283,4,0)+data


class AdapterCheck(unittest.TestCase):
    def probe(self, response, success):
        a,b = socket.socketpair(socket.AF_UNIX,socket.SOCK_SEQPACKET)
        a.settimeout(3)
        with tempfile.TemporaryDirectory() as directory:
            address = Path(directory)/'factory.bin'
            address.write_bytes(FACTORY)
            proc = subprocess.Popen([str(EXE),'7',str(address),str(b.fileno())],
                                    pass_fds=[b.fileno()],stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE,text=True)
            b.close()
            try:
                self.assertEqual(a.recv(4096),bytes.fromhex('040007000000'))
                a.send(response)
                out,err = proc.communicate(timeout=3)
                self.assertEqual(proc.returncode == 0,success,(out,err))
                self.assertEqual(a.recv(4096),b'', 'check attempted another command')
                self.assertNotIn('PRIVATE-adapter-name',out+err)
                self.assertNotIn(FACTORY.hex(),out+err)
                return out,err
            finally:
                if proc.poll() is None:
                    proc.kill(); proc.wait()
                a.close()

    def test_healthy_adapter_read_only(self):
        out,_ = self.probe(reply(),True)
        row=json.loads(out)
        self.assertTrue(row['adapter_check_ok'])
        self.assertTrue(row['powered'])
        self.assertTrue(row['bredr_supported'])
        self.assertTrue(row['le_supported'])
        self.assertFalse(row['connectable'])

    def test_unsafe_settings_or_wrong_identity_rejected(self):
        for response in (reply(settings=0x280),reply(settings=0x283),
                         reply(settings=0x289),reply(settings=0x681),
                         reply(address=bytes(6)),reply(maker=1),reply(version=7),
                         reply(supported=0x81)):
            with self.subTest(response=response[:30]):
                self.probe(response,False)

    def test_status_or_malformed_reply_rejected(self):
        for response in (struct.pack('<HHHHB',2,7,3,4,0x11),reply()[:-1],
                         struct.pack('<HHH',1,7,5000)+bytes(5000)):
            with self.subTest(length=len(response)):
                self.probe(response,False)


if __name__ == '__main__':
    unittest.main()
