#!/usr/bin/env python3
"""Sign or verify a Play AAB with the explicitly configured upload key."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[1]


def upload_config():
    names = ("ANDROID_UPLOAD_KEYSTORE_PATH", "ANDROID_UPLOAD_KEYSTORE_PASSWORD",
             "ANDROID_UPLOAD_KEY_ALIAS", "ANDROID_UPLOAD_KEY_PASSWORD")
    missing = [name for name in names if not os.environ.get(name)]
    if missing:
        raise ValueError("Missing required env: " + ", ".join(missing))
    keystore = Path(os.environ[names[0]]).expanduser()
    if not keystore.is_absolute():
        keystore = ROOT / keystore
    if not keystore.is_file():
        raise ValueError(f"ANDROID_UPLOAD_KEYSTORE_PATH does not point to a file: {keystore}")
    return keystore, os.environ["ANDROID_UPLOAD_KEY_ALIAS"]


def jarsigner(*arguments):
    java_home = os.environ.get("JAVA_HOME")
    executable = str(Path(java_home) / "bin/jarsigner") if java_home else "jarsigner"
    result = subprocess.run([executable, "-J-Duser.language=en", "-J-Duser.country=US",
                             *map(str, arguments)], text=True, capture_output=True)
    if result.returncode:
        raise ValueError("AAB signing/verification failed:\n" + result.stdout + result.stderr)
    return result.stdout


def verify(bundle, keystore, alias):
    output = jarsigner("-verify", "-strict", "-keystore", keystore,
                       "-storepass:env", "ANDROID_UPLOAD_KEYSTORE_PASSWORD", bundle, alias)
    # jarsigner returns success for a completely unsigned archive; require its
    # positive verification result as well as strict certificate/entry checks.
    if "jar verified." not in output:
        raise ValueError("AAB is not signed with the configured upload key.")


def is_signature_entry(name):
    parts = name.upper().split("/")
    return (len(parts) == 2 and parts[0] == "META-INF" and
            (parts[1].endswith((".SF", ".RSA", ".DSA", ".EC")) or parts[1].startswith("SIG-")))


def sign(bundle, keystore, alias):
    # Keep Gradle's payload and manifest attributes, removing the old signer's
    # signature files before jarsigner regenerates digests for the upload key.
    # Replace the output only after verification; a failure leaves it intact.
    with tempfile.TemporaryDirectory(prefix=".upload-signing-", dir=bundle.parent) as temporary:
        unsigned = Path(temporary) / "unsigned.aab"
        signed = Path(temporary) / "signed.aab"
        with zipfile.ZipFile(bundle) as source, zipfile.ZipFile(unsigned, "w") as target:
            target.comment = source.comment
            for entry in source.infolist():
                if not is_signature_entry(entry.filename):
                    with source.open(entry) as reader, target.open(entry, "w") as writer:
                        shutil.copyfileobj(reader, writer, 1024 * 1024)
        jarsigner("-keystore", keystore,
                  "-storepass:env", "ANDROID_UPLOAD_KEYSTORE_PASSWORD",
                  "-keypass:env", "ANDROID_UPLOAD_KEY_PASSWORD",
                  "-digestalg", "SHA-256", "-sigalg", "SHA256withRSA", "-sigfile", "UPLOAD",
                  "-signedjar", signed, unsigned, alias)
        verify(signed, keystore, alias)
        os.replace(signed, bundle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("bundle", type=Path)
    args = parser.parse_args()
    keystore, alias = upload_config()
    if not args.bundle.is_file() or args.bundle.stat().st_size == 0:
        raise ValueError(f"AAB not found or empty: {args.bundle}")
    if args.verify_only:
        verify(args.bundle, keystore, alias)
    else:
        sign(args.bundle, keystore, alias)
    print(f"Verified AAB upload signature: {args.bundle}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, zipfile.BadZipFile) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
