"""Ensure every shipped notice is readable offline and catalogued."""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

class LegalNoticesTests(unittest.TestCase):
    def test_catalog_matches_bundled_notices(self):
        catalog = (ROOT / 'src/legal/Notices.inc').read_text()
        files = re.findall(r'\{"([^\"]+\.txt)",', catalog)
        self.assertEqual(len(files), len(set(files)))
        self.assertEqual(set(files), {p.name for p in (ROOT / 'assets/legal').glob('*.txt')})
        for name in files:
            data = (ROOT / 'assets/legal' / name).read_bytes()
            self.assertGreater(len(data), 100, name)
            self.assertLess(len(data), 1024 * 1024, name)
            self.assertNotIn('\x00', data.decode('utf-8'), name)
        self.assertEqual((ROOT / 'assets/legal/asobmashow.txt').read_bytes(), (ROOT / 'LICENSE').read_bytes())
        for name in ['sdl', 'sdl-ttf', 'bgfx', 'bx', 'bimg', 'yoga', 'pcre2', 'miniaudio', 'stb-image', 'sol2', 'libsndfile', 'translation']:
            self.assertIn(name + '.txt', files)

if __name__ == '__main__':
    unittest.main()
