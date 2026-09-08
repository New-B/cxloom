"""Run 16 independent hosts against a fresh file-backed shared region."""
import os
import subprocess
import sys
import tempfile
import time

with tempfile.TemporaryDirectory(prefix="cxloom-par-") as directory:
    path = os.path.join(directory, "region")
    open(path, "wb").close()
    processes = []
    logs = []
    try:
        for host in range(16):
            log = open(os.path.join(directory, str(host)), "w+")
            logs.append(log)
            env = dict(os.environ, CL_DAX_DEVICE=path, CL_HOST_ID=str(host), CL_CREATE_REGION_FILE="1",
                       CL_HOST_COUNT="16", CL_PAR_ROUNDS=os.environ.get("CL_PAR_TEST_ROUNDS", "3"),
                       CL_PAR_CREATORS="4", CL_PAR_ROUND_DELAY_MS=os.environ.get("CL_PAR_TEST_DELAY_MS", "0"))
            processes.append(subprocess.Popen([sys.argv[1]], env=env, stdout=log, stderr=subprocess.STDOUT))
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
            print(log.read())
            log.close()
