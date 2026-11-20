"""UART guard lifecycle tests using PTYs and synthetic helper programs only."""
import os
from pathlib import Path
import pty
import signal
import subprocess
import tempfile
import termios
import time
import unittest

EXE = Path(os.environ.get('J98A_PROBE_BIN_DIR', Path(__file__).parent)) / 'hci-uart-supervisor-host'
PREFIX = '''#!/usr/bin/python3
import os,sys,termios,time,signal
from pathlib import Path
assert any(os.readlink(p)==sys.argv[1] for p in Path('/proc/%d/fd'%os.getppid()).iterdir())
fd=os.open(sys.argv[1],os.O_RDWR|os.O_NOCTTY)
t=termios.tcgetattr(fd)
assert t[4]==termios.B115200 and t[5]==termios.B115200
assert t[2]&termios.CRTSCTS and not t[2]&termios.HUPCL
'''


class SupervisorTests(unittest.TestCase):
    def run_case(self, patch_code, managed_code, expected=0, terminate=False, factory_code=None):
        master, slave = pty.openpty()
        before = termios.tcgetattr(slave)
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            patch=root/'patch'
            managed=root/'managed'
            patch.write_text(PREFIX+patch_code)
            managed.write_text(PREFIX+managed_code)
            patch.chmod(0o755)
            managed.chmod(0o755)
            args=[str(EXE),os.ttyname(slave),'unused-fw','unused-factory',str(patch),str(managed)]
            if factory_code is not None:
                factory=root/'factory'
                factory.write_text(PREFIX+factory_code)
                factory.chmod(0o755)
                args.append(str(factory))
            p=subprocess.Popen(args,
                               cwd=root,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            try:
                if terminate:
                    end=time.monotonic()+2
                    while not (root/'ready').exists():
                        self.assertLess(time.monotonic(),end)
                        time.sleep(.01)
                    p.send_signal(signal.SIGTERM)
                output,error=p.communicate(timeout=4)
                self.assertEqual(p.returncode,expected,(output,error))
                self.assertEqual(termios.tcgetattr(slave),before)
                return output,error,(root/'managed-called').exists(),(root/'cleaned').exists()
            finally:
                if p.poll() is None:
                    p.kill()
                    p.wait()
                os.close(master)
                os.close(slave)

    def test_continuous_fd_and_clean_exit(self):
        out,_,called,_=self.run_case("os.close(fd)\n", "Path('managed-called').touch()\nos.close(fd)\n")
        self.assertTrue(called)
        self.assertIn('after_managed',out)

    def test_patch_failure_skips_managed(self):
        _,_,called,_=self.run_case("sys.exit(9)\n", "Path('managed-called').touch()\n",9)
        self.assertFalse(called)

    def test_changed_baud_stops_handoff(self):
        _,_,called,_=self.run_case("t[4]=t[5]=termios.B9600\ntermios.tcsetattr(fd,termios.TCSANOW,t)\n", "Path('managed-called').touch()\n",1)
        self.assertFalse(called)

    def test_optional_factory_stage_runs_after_patch_before_managed(self):
        out,_,called,_=self.run_case(
            "Path('patch-complete').touch()\nos.close(fd)\n",
            "assert Path('factory-complete').exists()\nPath('managed-called').touch()\nos.close(fd)\n",
            factory_code="assert Path('patch-complete').exists()\nassert sys.argv[2]=='unused-factory'\nPath('factory-complete').touch()\nos.close(fd)\n")
        self.assertTrue(called)
        self.assertIn('after_factory_address',out)

    def test_factory_failure_prevents_managed_handoff(self):
        _,_,called,_=self.run_case("os.close(fd)\n","Path('managed-called').touch()\n",
                                  expected=7,factory_code="sys.exit(7)\n")
        self.assertFalse(called)

    def test_sigterm_waits_for_child_cleanup(self):
        managed="""def stop(sig,frame):
    os.close(fd)
    Path('cleaned').touch()
    sys.exit(0)
signal.signal(signal.SIGTERM,stop)
Path('ready').touch()
while True: time.sleep(.1)
"""
        _,_,_,cleaned=self.run_case("os.close(fd)\n",managed,143,True)
        self.assertTrue(cleaned)


if __name__ == '__main__':
    unittest.main()
