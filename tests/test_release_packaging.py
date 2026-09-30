"""Cross-platform tests for source isolation, ZIP contents and safe release names."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('package_release', ROOT / 'scripts/package_release.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class PackagingTests(unittest.TestCase):
    def test_version_validation(self):
        self.assertEqual(package.validate_version('v1.9.5'), '1.9.5')
        self.assertEqual(package.validate_version('1.9.5-rc.1'), '1.9.5-rc.1')
        for bad in ('', '../1.9.5', 'v1.9', 'v1.9.5\n', 'v1.9.5/evil', 'v256.0.0', 'v1.0.65536', 'v1.9.5"'):
            with self.subTest(version=bad), self.assertRaises(ValueError):
                package.validate_version(bad)

    def test_stamp_is_exact_and_preserves_other_bytes(self):
        data = b'// \xff\r\nstring GetVersion() {\r\n return "old";\r\n}\r\n'
        self.assertEqual(package.stamp_script(data, '1.9.5'), data.replace(b'"old"', b'"1.9.5"'))
        with self.assertRaises(ValueError):
            package.stamp_script(b'no version', '1.9.5')
        with self.assertRaises(ValueError):
            package.stamp_script(data + data, '1.9.5')

    def test_staging_and_archive_are_current_and_reproducible(self):
        originals = {name: (ROOT / name).read_bytes() for name in package.PLUGINS}
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp); stage = temp / 'stage'; first = temp / 'first'; second = temp / 'second'
            metadata = package.stage_source(ROOT, stage, 'v1.9.5')
            self.assertEqual(metadata['version'], '1.9.5')
            self.assertFalse((stage / 'releases/latest').exists())
            self.assertFalse(list(stage.rglob('*.exe')))
            for name in ('Package.wxs', 'Package.en-us.wxl', 'TargetValidation.cpp'):
                self.assertEqual((stage / 'installer/msi' / name).read_bytes(), (ROOT / 'installer/msi' / name).read_bytes())
            for name, original in originals.items():
                self.assertEqual((ROOT / name).read_bytes(), original)
                self.assertEqual((stage / name).read_bytes(), package.stamp_script(original, '1.9.5'))
            files = package.finalize(stage, first)
            package.finalize(stage, second)
            for path in files:
                self.assertEqual(path.read_bytes(), (second / path.name).read_bytes())
            archive_path = next(first.glob('*.zip'))
            with zipfile.ZipFile(archive_path) as archive:
                self.assertEqual(set(archive.namelist()), set((*package.PAYLOAD, *package.DOCS, 'BUILD-INFO.json', 'INSTALL.txt')))
                self.assertIn(b'thinking=disabled', archive.read('INSTALL.txt'))
                for name in package.PLUGINS:
                    self.assertEqual(hashlib.sha256(archive.read(name)).hexdigest(), metadata['plugin_sha256'][name])
            for line in (first / 'SHA256SUMS').read_text().splitlines():
                digest, name = line.split('  ')
                self.assertEqual(digest, hashlib.sha256((first / name).read_bytes()).hexdigest())
            with self.assertRaises(ValueError):
                package.stage_source(ROOT, stage, 'v1.9.5')
            (first / 'old-installer.exe').write_bytes(b'old')
            with self.assertRaises(ValueError):
                package.finalize(stage, first)

    def test_finalization_requires_fresh_all_format_installers(self):
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp); stage = temp / 'stage'; output = temp / 'output'; output.mkdir()
            package.stage_source(ROOT, stage, 'v1.9.5')
            with self.assertRaises(FileNotFoundError):
                package.finalize(stage, output, True)
            for name in package.INSTALLERS:
                (output / name).write_bytes(b'not an installer')
            with self.assertRaises(ValueError):
                package.finalize(stage, output, True)


if __name__ == '__main__':
    unittest.main()
