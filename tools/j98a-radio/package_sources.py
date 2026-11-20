"""Copy an explicit source-only allowlist; no firmware, binaries or device logs."""
import hashlib
import json
from pathlib import Path
import shutil
import sys

SOURCE = Path(__file__).resolve().parent
FILES = (
    'LICENSE', 'README.rst', 'build.sh', 'build-metadata.json',
    'h4_probe.c', 'hci_attach_probe.c', 'j98a_patchram.h',
    'hci_managed_session.c', 'hci_uart_supervisor.c', 'j98a-managed-session',
    'hci_metadata_monitor.c', 'test_hci_monitor.py',
    'hci_adapter_check.c', 'test_hci_adapter_check.py',
    'hci_factory_address.c', 'test_hci_factory_address.py',
    'test_h4_probe.py', 'test_hci_attach_probe.py', 'test_hci_patchram.py',
    'test_hci_managed.py', 'test_uart_supervisor.py', 'package_sources.py',
)


def main():
    output = Path(sys.argv[1]) if len(sys.argv) > 1 else SOURCE/'source-package'
    output.mkdir(parents=True, exist_ok=True)
    extras = {p.name for p in output.iterdir()} - set(FILES) - {'source-manifest.json'}
    if extras:
        raise SystemExit(f'Refusing nonempty destination with unrelated files: {sorted(extras)}')
    manifest = {}
    for name in FILES:
        source = SOURCE/name
        data = source.read_bytes()
        if data.startswith(b'\x7fELF') or b'\0' in data:
            raise SystemExit(f'Non-text source rejected: {name}')
        shutil.copyfile(source, output/name)
        (output/name).chmod(0o755 if name in ('build.sh', 'j98a-managed-session') else 0o644)
        manifest[name] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    (output/'source-manifest.json').write_text(json.dumps({'schema': 1, 'files': manifest}, indent=2)+'\n')
    print(f'Copied {len(FILES)} text source/test/metadata files to {output}')


if __name__ == '__main__':
    main()
