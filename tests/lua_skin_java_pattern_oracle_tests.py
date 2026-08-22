#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile
import textwrap
import unittest


CASES = (
    (r"(?i:flag_only\.txt)", "FLAG_ONLY.TXT"),
    (r"file_(?i)SOURCE\.TXT", "file_source.txt"),
    (r"file_(?<=file_)source", "file_source.txt"),
    (r"(?<=(file_))target_\1\.txt", "file_target_file_.txt"),
    (r"(?<stem>file)_\k<stem>\.txt", "file_file.txt"),
    (r"(?>file)_source\.txt", "file_source.txt"),
    (r"file_+source\.txt", "file_source.txt"),
    (r"(?U)(?<![\p{L}\p{N}_])\w+\.txt$", "Élan.txt"),
    (r"(?iu)é", "É"),
    (r"(?m)^b$", "a\rb"),
    (r"(?dm)^b$", "a\rb"),
    (r"(?i)A(?-i)b", "ab"),
    ("(?x)a  # ignored\n b", "ab"),
    (r"(?i)", "anything"),
    (r"(?:\Q[literal]\E)[.]txt", "[literal].txt"),
    (r"(?<=*)", "anything"),
)

NATIVE_EXECUTABLE = None


JAVA_SOURCE = r"""
import java.nio.charset.StandardCharsets;
import java.util.regex.Pattern;
import java.util.regex.PatternSyntaxException;

public final class LuaSkinJavaPatternOracle {
  public static void main(String[] args) {
    try {
      var matcher = Pattern.compile(args[0]).matcher(args[1]);
      if (!matcher.find()) {
        System.out.println("NO_MATCH");
        return;
      }
      var bytes = matcher.group().getBytes(StandardCharsets.UTF_8);
      var result = new StringBuilder("MATCH:");
      for (byte value : bytes) {
        result.append(String.format("%02x", value & 0xff));
      }
      System.out.println(result);
    } catch (PatternSyntaxException error) {
      System.out.println("INVALID");
    }
  }
}
"""


class LuaSkinJavaPatternOracleTests(unittest.TestCase):
    def test_native_matcher_agrees_with_java_pattern(self):
        if NATIVE_EXECUTABLE is None:
            self.fail("native matcher executable argument is required")
        native = NATIVE_EXECUTABLE
        self.assertTrue(native.is_file(), native)

        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            source = directory / "LuaSkinJavaPatternOracle.java"
            source.write_text(textwrap.dedent(JAVA_SOURCE), encoding="utf-8")
            subprocess.run(["javac", str(source)], check=True)

            for pattern, subject in CASES:
                with self.subTest(pattern=pattern, subject=subject):
                    java = subprocess.run(
                        [
                            "java",
                            "-cp",
                            str(directory),
                            "LuaSkinJavaPatternOracle",
                            pattern,
                            subject,
                        ],
                        check=True,
                        capture_output=True,
                        text=True,
                    ).stdout.strip()
                    actual = subprocess.run(
                        [str(native), pattern, subject],
                        check=True,
                        capture_output=True,
                        text=True,
                    ).stdout.strip()
                    self.assertEqual(actual, java)


if __name__ == "__main__":
    executable = sys.argv[1:2]
    NATIVE_EXECUTABLE = pathlib.Path(executable[0]).resolve() if executable else None
    sys.argv = [sys.argv[0]]
    unittest.main()
