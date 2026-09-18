"""Run 16 independent hosts against a fresh file-backed shared region."""
import os
import re
import subprocess
import sys
import tempfile
import time

with tempfile.TemporaryDirectory(prefix="cxloom-par-") as directory:
    path = os.path.join(directory, "region")
    open(path, "wb").close()
    processes = []
    logs = []
    outputs = []
    try:
        for host in range(16):
            log = open(os.path.join(directory, str(host)), "w+")
            logs.append(log)
            env = dict(os.environ, CL_DAX_DEVICE=path, CL_HOST_ID=str(host), CL_CREATE_REGION_FILE="1",
                       CL_HOST_COUNT="16", CL_PAR_ROUNDS=os.environ.get("CL_PAR_TEST_ROUNDS", "3"),
                       CL_PAR_CREATORS="4", CL_PAR_ROUND_DELAY_MS=os.environ.get("CL_PAR_TEST_DELAY_MS", "0"))
            processes.append(subprocess.Popen([sys.argv[1], *sys.argv[2:]], env=env, stdout=log, stderr=subprocess.STDOUT))
            if host == 0:
                deadline = time.monotonic() + 20
                while True:
                    log.seek(0)
                    if "region ready" in log.read():
                        break
                    if processes[0].poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError("owner failed to initialize")
                    time.sleep(0.01)
        for process in processes:
            if process.wait(timeout=int(os.environ.get("CL_PAR_TEST_TIMEOUT_SECONDS", "120"))) != 0:
                raise RuntimeError("host failed")
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
            process.wait()
        for log in logs:
            log.seek(0)
            output = log.read()
            outputs.append(output)
            print(output)
            log.close()

    expected = os.environ.get("CL_PAR_EXPECT_PRIMARY_INVOCATIONS")
    if expected:
        rows = [tuple(map(int, match)) for match in re.findall(
            r"remote verification host=(\d+) remote_results=(\d+) executed=(\d+) remote_executed=(\d+)",
            "\n".join(outputs))]
        if len(rows) != 16 or {row[0] for row in rows} != set(range(16)):
            raise RuntimeError("missing per-host remote execution counters")
        if sum(row[2] for row in rows) != int(expected):
            raise RuntimeError("missing or duplicate primary invocation execution")
        if any(row[1] == 0 for row in rows) or sum(row[1] for row in rows) != sum(row[3] for row in rows):
            raise RuntimeError("remote result and execution counters disagree")
