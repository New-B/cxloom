# Phase 8 evaluation — 2026-09-06

## DAX acceptance attempt

Ran `./scripts/run-loompar-containers.sh` with defaults (16 containers,
300 rounds, four creators, 1000 ms delay). Preflight failed while inspecting
`cxloom-h0`: access to `/var/run/docker.sock` returned `operation not permitted`.
No workload started and no DAX region was formatted. Execution and synchronization
container acceptance are both still pending. This environment disallows privilege
escalation. Retry execution and then `CL_PAR_PROGRAM=cxloom_loompar_sync
./scripts/run-loompar-containers.sh` in an environment with Docker access;
run them sequentially because each bootstrap owner formats the region.

## Placement evaluation

Build and run:

```bash
cmake -S . -B build
cmake --build build -j 4
build/cxloom_loompar_scheduler_bench
ctest --test-dir build --output-on-failure --timeout 180
```

The benchmark uses the production placement scheduler in a deterministic,
single-process discrete workload simulation: 16 single-server hosts, 4096 tasks,
arrival spacing 0.25 model units, service time 2 units (40 for every seventh task
in the skewed workload). Queue pressure adds 8 units of transport delay on hosts
0–3. Compare round-robin, least outstanding task count, and weighted placement.
All policies see the same arrivals and task service requirements. Weighted mode
uses queue weight 1, history weight 0, no locality hint and current load samples;
it isolates the queue term rather than evaluating every production default.

| Workload | Policy | Mean latency | P95 latency | Makespan |
| --- | --- | ---: | ---: | ---: |
| Uniform | Round-robin | 2 | 2 | 1025.75 |
| Uniform | Least-loaded / weighted | 2 | 2 | 1025.75 |
| Skewed | Round-robin | 451.161 | 848 | 1927.75 |
| Skewed | Least-loaded / weighted | 443.180 | 903 | 2174.25 |
| Queue pressure | Round-robin | 4 | 10 | 1030.75 |
| Queue pressure | Least-loaded | 2.80469 | 10 | 1030.75 |
| Queue pressure | Weighted | 2 | 2 | 1025.75 |

All numbers are simulated units, not measured DAX performance. Queue-aware
placement avoids the modeled transport backlog. Counting tasks fails to predict
remaining service demand in the skewed workload: its tail latency and makespan
are worse than round-robin. This is evidence for adding measured service-time
estimates, not evidence that the current scheduler universally improves speed.
The simulator does not model real worker pools, cache/token movement, telemetry
staleness or migration overhead. Local CSV: `run/phase8-20260906/scheduler.csv`.

Fixed fractional score truncation (0.25 must beat 0.75) and corrected exponential
aging to use the configured half-life. Added a regression for fractional scores.
Full regression after changes: **22/22 passed**, including both 16-process
file-backed workloads; elapsed 9.75 seconds. `git diff --check` passed.

## Migration and remaining Phase 8 acceptance

Added a home-owned migration transaction model with epoch/endpoint checks,
quiescence, preparation, commit, resume acknowledgement and precommit rollback.
Tests reject stale messages and postcommit rollback. See
[the migration protocol](loompar-migration-protocol.md) for integration obligations.
The model is not wired to runtime transport; cross-process continuation transfer
is still unimplemented and no claim of successful migration is made.

Remaining measurements: real 16-host create/join latency and throughput under
automatic placement, per-host utilization, actual token transfers/bytes,
locality and history ablations, stale telemetry, and continuation migration
cost compared with moving token/data. Complete distributed continuation and
cancellation tests before enabling migration handlers.
