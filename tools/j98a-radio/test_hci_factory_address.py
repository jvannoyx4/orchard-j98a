"""Factory provisioning uses only synthetic addresses and local socketpairs."""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
EXE = Path(os.environ.get('J98A_PROBE_BIN_DIR', HERE))/'hci-factory-address-test'
FACTORY = bytes.fromhex('102030405060')
WIRE = FACTORY[::-1]
VERSION = bytes.fromhex('01011000')
READ_ADDRESS = bytes.fromhex('01091000')
WRITE_ADDRESS = bytes.fromhex('0101fc06')+WIRE
PATCHED_VERSION = bytes.fromhex('00083e13080f000766')


def complete(opcode, data=b'\0', credit=1):
    body=struct.pack('<BH',credit,opcode)+data
    return bytes([4,0x0e,len(body)])+body


class FactoryAddress(unittest.TestCase):
    def probe(self, respond, success=False, contents=FACTORY, mode=0o600, symlink=False):
        a,b=socket.socketpair(socket.AF_UNIX,socket.SOCK_SEQPACKET)
        a.settimeout(3)
        with tempfile.TemporaryDirectory() as directory:
            address=Path(directory)/'factory.bin'
            address.write_bytes(contents)
            address.chmod(mode)
            if symlink:
                link=Path(directory)/'link.bin'
                link.symlink_to(address)
                address=link
            proc=subprocess.Popen([str(EXE),str(b.fileno()),str(address)],
                                  pass_fds=[b.fileno()],stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE,text=True)
            b.close()
            try:
                if respond:
                    self.assertEqual(a.recv(4096),VERSION)
                    respond(a)
                out,err=proc.communicate(timeout=4)
                self.assertEqual(proc.returncode == 0,success,(out,err))
                self.assertEqual(a.recv(4096),b'', 'unexpected additional HCI command')
                self.assertNotIn(FACTORY.hex(),out+err)
                self.assertNotIn(':'.join(f'{b:02x}' for b in FACTORY),out+err)
                self.assertNotIn(WIRE.hex(),out+err)
                return out,err
            finally:
                if proc.poll() is None:
                    proc.kill(); proc.wait()
                a.close()

    def identify_and_read(self, sock, current=bytes(6)):
        sock.send(complete(0x1001,PATCHED_VERSION))
        self.assertEqual(sock.recv(4096),READ_ADDRESS)
        sock.send(complete(0x1009,b'\0'+current))

    def test_writes_exact_reversed_factory_and_verifies(self):
        def respond(sock):
            self.identify_and_read(sock)
            self.assertEqual(sock.recv(4096),WRITE_ADDRESS)
            sock.send(complete(0xfc01))
            self.assertEqual(sock.recv(4096),READ_ADDRESS)
            sock.send(complete(0x1009,b'\0'+WIRE))
        out,_=self.probe(respond,True)
        self.assertTrue(json.loads(out.splitlines()[-1])['factory_address_verified'])

    def test_already_matches_does_not_write(self):
        def respond(sock):
            self.identify_and_read(sock,WIRE)
            self.assertEqual(sock.recv(4096),READ_ADDRESS)
            sock.send(complete(0x1009,b'\0'+WIRE))
        out,_=self.probe(respond,True)
        self.assertFalse(json.loads(out.splitlines()[-1])['runtime_write_performed'])

    def test_readback_mismatch_stops_without_retry(self):
        def respond(sock):
            self.identify_and_read(sock)
            self.assertEqual(sock.recv(4096),WRITE_ADDRESS)
            sock.send(complete(0xfc01))
            self.assertEqual(sock.recv(4096),READ_ADDRESS)
            sock.send(complete(0x1009,b'\0'+FACTORY))  # Wrong byte order.
        self.probe(respond)

    def test_write_error_or_missing_credit_stops(self):
        for ack in (complete(0xfc01,b'\x01'),complete(0xfc01,credit=0),complete(0xfc01)[:-1]):
            def respond(sock):
                self.identify_and_read(sock)
                self.assertEqual(sock.recv(4096),WRITE_ADDRESS)
                sock.send(ack)
            with self.subTest(ack=ack):
                self.probe(respond)

    def test_wrong_patch_revision_or_chip_prevents_address_access(self):
        for position in (1,2,3,4,5,7,8):
            version=bytearray(PATCHED_VERSION)
            version[position]^=1
            with self.subTest(position=position):
                self.probe(lambda s: s.send(complete(0x1001,version)))

    def test_invalid_files_rejected_before_transport(self):
        for contents in (b'',FACTORY[:-1],FACTORY+b'\0',bytes(6),bytes([255])*6,
                         bytes.fromhex('112030405060')):
            with self.subTest(length=len(contents)):
                self.probe(None,contents=contents)
        self.probe(None,mode=0o644)
        self.probe(None,symlink=True)


if __name__ == '__main__':
    unittest.main()
