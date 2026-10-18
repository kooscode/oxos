#!/usr/bin/env python3
"""Download a file and check it against a pinned SHA-256 (the Makefile's third-party downloads).

usage: fetch.py URL SHA256 OUT

The file is written to OUT only when its SHA-256 matches, so an interrupted or changed download never leaves a
bad file behind. Uses only the Python standard library (no curl or wget needed).
"""
import hashlib, os, sys, tempfile, urllib.request


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__.split("\n\n")[1])
    url, want, out = sys.argv[1], sys.argv[2].lower(), sys.argv[3]
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    print(f"fetching {url}")
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(os.path.abspath(out)), prefix=".fetch-")
    try:
        h = hashlib.sha256()
        with os.fdopen(fd, "wb") as f, urllib.request.urlopen(url, timeout=60) as r:
            while chunk := r.read(1 << 16):
                h.update(chunk)
                f.write(chunk)
        if h.hexdigest() != want:
            sys.exit(f"{url}: SHA-256 {h.hexdigest()}, expected {want}; not saved")
        umask = os.umask(0)
        os.umask(umask)
        os.chmod(tmp, 0o666 & ~umask)  # a normal file, not mkstemp's private one
        os.replace(tmp, out)
    except OSError as e:
        sys.exit(f"{url}: download failed ({e})")
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


if __name__ == "__main__":
    main()
