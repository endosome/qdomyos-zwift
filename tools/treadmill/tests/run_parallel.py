"""Run the real-time fake-peer tests of one unittest file concurrently.

These tests spend nearly all their time waiting on the CLI's real hold and settle windows, so running each test in
its own process (every test uses a private temporary directory) cuts the wall time to about the slowest test.
usage: run_parallel.py TEST_FILE BINARY [JOBS]
"""
import subprocess
import sys
import unittest
import importlib.util
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

test_file, binary = Path(sys.argv[1]).resolve(), sys.argv[2]
jobs = int(sys.argv[3]) if len(sys.argv) > 3 else 12


def test_ids():
    saved = sys.argv[:]
    sys.argv = [str(test_file), binary]
    try:
        spec = importlib.util.spec_from_file_location(test_file.stem, test_file)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    finally:
        sys.argv = saved
    def walk(suite):
        for item in suite:
            if isinstance(item, unittest.TestSuite):
                yield from walk(item)
            else:
                yield item.id().split('.', 1)[1] if '.' in item.id() and item.id().startswith(test_file.stem + '.') else item.id()
    return list(walk(unittest.defaultTestLoader.loadTestsFromModule(module)))


def run(test_id):
    result = subprocess.run([sys.executable, str(test_file), binary, test_id], capture_output=True, text=True)
    return test_id, result.returncode, result.stderr


ids = test_ids()
with ThreadPoolExecutor(jobs) as pool:
    results = list(pool.map(run, ids))
failed = [r for r in results if r[1] != 0]
for test_id, _, errors in failed:
    print('FAILED', test_id, '\n', errors[-3000:], file=sys.stderr)
print('Ran %d tests in parallel, %d failed' % (len(results), len(failed)))
sys.exit(1 if failed else 0)
