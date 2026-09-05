#!/usr/bin/env python3
# noise_vectors.py - run cacophony known-answer vectors through tests/noise_kat.
#
# Vectors are vendored in tests/vectors/ (subset of cacophony, see source
# field in each file) so tests run offline. Usage: python3 tests/noise_vectors.py
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
KAT = os.path.join(HERE, "noise_kat")
VECTORS_DIR = os.path.join(HERE, "vectors")


def hex_or_dash(hexstr):
    return hexstr if hexstr else "-"


def run_vector(vec, workdir):
    spec_path = os.path.join(workdir, "spec.txt")
    with open(spec_path, "w") as f:
        f.write("init_static %s\n" % vec["init_static"])
        f.write("init_ephemeral %s\n" % vec["init_ephemeral"])
        f.write("resp_static %s\n" % vec["resp_static"])
        f.write("resp_ephemeral %s\n" % vec["resp_ephemeral"])
        # both parties see the same prologue in cacophony; our init mixes
        # one prologue, so assert they match before handing to the harness
        if vec.get("init_prologue", "") != vec.get("resp_prologue", ""):
            return "SKIP (asymmetric prologue)"
        f.write("prologue %s\n" % hex_or_dash(vec.get("init_prologue", "")))
        for i, m in enumerate(vec["messages"]):
            direction = "i" if i % 2 == 0 else "r"  # cacophony alternates i,r,...
            f.write("msg %s %s %s\n"
                    % (direction, hex_or_dash(m["payload"]),
                       hex_or_dash(m["ciphertext"])))
    proc = subprocess.run([KAT, spec_path], capture_output=True, text=True)
    if proc.returncode != 0:
        return "FAIL: " + (proc.stderr.strip() or proc.stdout.strip())
    return "PASS"


def main():
    if not os.path.isfile(KAT):
        sys.exit("harness not built: run 'make tests/noise_kat' first")
    files = sorted(f for f in os.listdir(VECTORS_DIR) if f.endswith(".json"))
    if not files:
        sys.exit("no vectors found in " + VECTORS_DIR)
    total = passed = 0
    with tempfile.TemporaryDirectory() as workdir:
        for fn in files:
            doc = json.load(open(os.path.join(VECTORS_DIR, fn)))
            for vec in doc["vectors"]:
                total += 1
                result = run_vector(vec, workdir)
                if result == "PASS":
                    passed += 1
                print("%-45s %s" % (vec["name"], result))
    print("%d/%d vectors passed" % (passed, total))
    sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
    main()
