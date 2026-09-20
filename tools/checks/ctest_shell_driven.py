"""Names of the ctest entries that only drive the built binary.

Reads `ctest --show-only=json-v1` on stdin and prints the tests whose command is
a shell script rather than a test executable: those run against a tree that
built the driver alone (`just dev`), which is what lets the landing gate carry
them. Anything else needs its own target built and belongs to the full ctest
phase, which the gate build and CI's build job run.
"""

import json
import sys

listing = json.load(sys.stdin)
for test in listing.get("tests", []):
    command = test.get("command") or []
    if command and command[0].rsplit("/", 1)[-1] in ("bash", "sh"):
        print(test["name"])
