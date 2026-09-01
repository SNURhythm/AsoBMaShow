#!/usr/bin/env python3

import os
import pathlib
import re
import shutil
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
    (r"(?i)é", "É"),
    (r"(?U:\w)\w", "éé"),
    (r"(?U:\w(?-U:\w)\w)", "éaé"),
    (r"(?U:(?-U:\w)\w)", "aé"),
    (r"^\w+$", "é"),
    (r"^(?U:\w+)$", "é"),
    (r"(?U:\d)(?-U:\d)", "١1"),
    (r"(?U:\d)(?-U:\d)", "١١"),
    (r"(?iu:é)(?i-u:é)", "ÉÉ"),
    (r"[a-z&&[^m-p]]+", "abcnoq"),
    (r"[a-z&&[^m-p]&&[^d-f]]+", "abcdeq"),
    (r"[[a-f]&&[d-z]]+", "abcdex"),
    (r"[a-z&&[def]]+", "abcde"),
    (r"[a-z&&[^aeiou]]+", "aeiobcdf"),
    (r"(?i:[a-z&&[^m-p]]+)", "ABCNOQ"),
    (r"(?<=([a-z&&[^m-p]]))\1", "aa"),
    (r"(?<stem>[a-z&&[^m-p]])-\k<stem>", "q-q"),
    (r"^(?i:[éa]+)$", "ÉA"),
    (r"(?i)\x{61}", "A"),
    (r"(?i)\x{e9}", "É"),
    (r"\bfoo\b", "éfooé"),
    (r"(?U:\bfoo\b)", "éfooé"),
    (r"(?i)\Qéa\E", "éA"),
    (r"(?i)\Qéa\E", "ÉA"),
    (r"(?i)(?<stem>a)\k<stem>", "aA"),
    (r"(?i)(?<stem>é)\k<stem>", "éÉ"),
    (r"(?i)([a-z&&[^m-p]])\1", "aA"),
    (r"(?m)^b$", "a\rb"),
    (r"(?dm)^b$", "a\rb"),
    (r"\bfoo", "‿foo"),
    (r"(?U:\bfoo)", "‿foo"),
    (r"\bfoo", "‌foo"),
    (r"(?U:\bfoo)", "‌foo"),
    (r"\bfoo", "́foo"),
    (r"(?U:\bfoo)", "́foo"),
    (r"\bfoo", "áfoo"),
    (r"(?U:\bfoo)", "áfoo"),
    (r"\bfoo", "a\U0001d167foo"),
    (r"(?U:\bfoo)", "a\U0001d167foo"),
    (r"a\b𝅧", "a𝅧"),
    (r"a\B𝅧", "a𝅧"),
    (r"\bfoo", "_́foo"),
    (r"\bfoo", "𐐀́foo"),
    (r"\bfoo", "²foo"),
    (r"(?U:\bfoo)", "²foo"),
    (r"\bfoo", "Ⓐfoo"),
    (r"(?U:\bfoo)", "Ⓐfoo"),
    (r"\bfoo", "🄰foo"),
    (r"(?U:\bfoo)", "🄰foo"),
    (r"\Bfoo", "‿foo"),
    (r"(?U:\Bfoo)", "‿foo"),
    (r"\Bfoo", "́foo"),
    (r"(?U:\Bfoo)", "́foo"),
    (r"\Bfoo", "áfoo"),
    (r"(?U:\Bfoo)", "áfoo"),
    (r"\Bfoo", "a\U0001d167foo"),
    (r"(?U:\Bfoo)", "a\U0001d167foo"),
    (r"\Bfoo", "Ⓐfoo"),
    (r"(?U:\Bfoo)", "Ⓐfoo"),
    (r"\Bfoo", "🄰foo"),
    (r"(?U:\Bfoo)", "🄰foo"),
    (r"(?i)A(?-i)b", "ab"),
    ("(?x)a  # ignored\n b", "ab"),
    (r"(?x)[a b]", " "),
    (r"(?x)[a b]", "b"),
    (r"(?x)[[a b]&&[^c ]]", " "),
    (r"(?x:[a b])(?-x:[ c])", "b "),
    (r"(?x)[a\ ]", " "),
    (r"^\p{Lower}+$", "é"),
    (r"(?U)^\p{Lower}+$", "é"),
    (r"(?U:\p{Lower})(?-U:\p{Lower})", "éa"),
    (r"(?U:\p{Lower})(?-U:\p{Lower})", "éé"),
    (r"^[\p{Lower}]+$", "é"),
    (r"(?U)^[\p{Lower}]+$", "é"),
    (r"^\p{Upper}+$", "É"),
    (r"(?U)^\p{Upper}+$", "É"),
    (r"^\p{Alpha}+$", "é"),
    (r"(?U)^\p{Alpha}+$", "é"),
    (r"^\p{Digit}+$", "١"),
    (r"(?U)^\p{Digit}+$", "١"),
    (r"^\p{Alnum}+$", "é١"),
    (r"(?U)^\p{Alnum}+$", "é١"),
    (r"^\p{Punct}+$", "—"),
    (r"(?U)^\p{Punct}+$", "—"),
    (r"^\p{Graph}+$", "é"),
    (r"(?U)^\p{Graph}+$", "é"),
    (r"^\p{Print}+$", "é"),
    (r"(?U)^\p{Print}+$", "é"),
    (r"^\p{Blank}+$", "\u00a0"),
    (r"(?U)^\p{Blank}+$", "\u00a0"),
    (r"^\p{Cntrl}+$", "\u0085"),
    (r"(?U)^\p{Cntrl}+$", "\u0085"),
    (r"^\p{XDigit}+$", "Ｆ"),
    (r"(?U)^\p{XDigit}+$", "Ｆ"),
    (r"^\p{Space}+$", "\u00a0"),
    (r"(?U)^\p{Space}+$", "\u00a0"),
    (r"^\P{Lower}+$", "é"),
    (r"(?U)^\P{Lower}+$", "é"),
    (r"^[\P{Lower}]+$", "é"),
    (r"(?U)^[\P{Lower}]+$", "é"),
    (r"(?i)", "anything"),
    (r"(?:\Q[literal]\E)[.]txt", "[literal].txt"),
    (r"(?<=*)", "anything"),
    (r"(?q:a)", "a"),
    (r"(?i--u:a)", "a"),
    (r"[a-z", "a"),
)

NATIVE_EXECUTABLE = None
TARGET_JAVA_MAJOR = 17


def _java_major(java):
    result = subprocess.run(
        [str(java), "-version"],
        check=True,
        capture_output=True,
        text=True,
    )
    version = re.search(
        r'(?:version "|javac\s+)(?:1\.)?(\d+)',
        result.stderr + result.stdout,
    )
    return int(version.group(1)) if version else None


def _java_home_candidates():
    configured = os.environ.get("ASOBMASHOW_JAVA_17_HOME")
    if configured:
        yield pathlib.Path(configured)

    java_home_tool = pathlib.Path("/usr/libexec/java_home")
    if sys.platform == "darwin" and java_home_tool.is_file():
        result = subprocess.run(
            [str(java_home_tool), "-v", str(TARGET_JAVA_MAJOR)],
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode == 0 and result.stdout.strip():
            yield pathlib.Path(result.stdout.strip())

    configured = os.environ.get("JAVA_HOME")
    if configured:
        yield pathlib.Path(configured)

    java = shutil.which("java")
    if java:
        yield pathlib.Path(java).resolve().parent.parent

    for candidate in sorted(pathlib.Path("/usr/lib/jvm").glob("*17*")):
        yield candidate


def _java_17_tools():
    executable_suffix = ".exe" if os.name == "nt" else ""
    visited = set()
    for home in _java_home_candidates():
        home = home.expanduser().resolve()
        if home in visited:
            continue
        visited.add(home)
        java = home / "bin" / f"java{executable_suffix}"
        javac = home / "bin" / f"javac{executable_suffix}"
        if not java.is_file() or not javac.is_file():
            continue
        try:
            if (
                _java_major(java) == TARGET_JAVA_MAJOR
                and _java_major(javac) == TARGET_JAVA_MAJOR
            ):
                return java, javac
        except (OSError, subprocess.CalledProcessError):
            continue
    raise RuntimeError(
        "Java 17 is required for the Beatoraja Pattern oracle; set "
        "ASOBMASHOW_JAVA_17_HOME to a JDK 17 installation"
    )


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
    def test_java_oracle_uses_a_consistent_java_17_toolchain(self):
        java, javac = _java_17_tools()
        self.assertEqual(_java_major(java), TARGET_JAVA_MAJOR)
        self.assertEqual(_java_major(javac), TARGET_JAVA_MAJOR)

    def test_native_matcher_bounds_group_depth_before_adaptation(self):
        if NATIVE_EXECUTABLE is None:
            self.fail("native matcher executable argument is required")
        native = NATIVE_EXECUTABLE
        self.assertTrue(native.is_file(), native)

        def native_result(depth):
            pattern = "^" + "(" * depth + "a" + ")" * depth + "$"
            return subprocess.run(
                [str(native), pattern, "a"],
                check=True,
                capture_output=True,
                text=True,
            ).stdout.strip()

        self.assertEqual(native_result(96), "MATCH:61")
        self.assertEqual(native_result(97), "INVALID")

    def test_native_matcher_agrees_with_java_pattern(self):
        if NATIVE_EXECUTABLE is None:
            self.fail("native matcher executable argument is required")
        native = NATIVE_EXECUTABLE
        self.assertTrue(native.is_file(), native)
        java_executable, javac_executable = _java_17_tools()

        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            source = directory / "LuaSkinJavaPatternOracle.java"
            source.write_text(textwrap.dedent(JAVA_SOURCE), encoding="utf-8")
            subprocess.run([str(javac_executable), str(source)], check=True)

            for pattern, subject in CASES:
                with self.subTest(pattern=pattern, subject=subject):
                    java_result = subprocess.run(
                        [
                            str(java_executable),
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
                    self.assertEqual(actual, java_result)


if __name__ == "__main__":
    executable = sys.argv[1:2]
    NATIVE_EXECUTABLE = pathlib.Path(executable[0]).resolve() if executable else None
    sys.argv = [sys.argv[0]]
    unittest.main()
