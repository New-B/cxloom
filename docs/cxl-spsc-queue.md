# CXL-Resident Host-Pair SPSC Queues

The LoomMem control plane uses one ring for every directed pair of distinct
hosts. A four-host deployment therefore has 12 independent queues. Self-pairs
stay on the local runtime path and do not consume shared queue space.

## Shared layout

The 64 MiB queue region begins with a versioned `QueueRegionHeader`. Each ring
then contains:

1. immutable producer, consumer, capacity, and slot-size identity;
2. a producer-owned tail cursor on its own cache line;
3. a consumer-owned head cursor on its own cache line;
4. fixed-size, cache-line-aligned slots with a monotonic sequence number,
   `MessageHeader`, and up to 128 bytes of inline payload.

Only the configured producer can call `Push`, and only the configured consumer
can call `Pop`. No shared CAS or MPMC ownership arbitration is used. Larger
application arguments must be stored as shared objects and referenced by GPtr
from a control message.

## Consumer poller

`LoomMemRuntime::StartQueuePoller` creates one dedicated consumer thread and caches all inbound queues for the local host. The thread is pinned to an explicit `QueuePollerOptions::cpu_id`, or to the first CPU in the process affinity mask when no CPU is specified. Deployments should reserve that CPU for the poller.

Each scan begins at a rotating queue index, so a persistently busy low-numbered producer cannot starve later queues. An active queue is drained up to `batch_size` messages before scanning the next queue. Empty scans use an adaptive `PAUSE -> sched_yield -> short sleep` policy; any received message immediately returns the poller to the hot-spin state.

Handlers execute serially on the poller thread and return `Status`. A queue or handler failure stops the poller and is exposed through `terminal_status` and `StopQueuePoller`. Runtime finalization always joins the poller before unmapping CXL memory. Statistics expose scans, empty scans, messages, batches, yields, sleeps, and the bound CPU.

## Publication protocol

A producer publishes a message in this order:

    slot header and payload
      -> PublishData(payload)
      -> release-store slot sequence
      -> PublishData(sequence)
      -> release-store producer tail
      -> PublishData(tail)

A consumer acquires the producer tail, acquires and verifies the expected slot
sequence, copies the payload, then release-publishes its head. Queue-full
backpressure is `tail - head == capacity`; queue-empty is `head == tail`.
The selected visibility profile is used at every cross-host publication and
acquisition point.

The bootstrap owner formats all rings before publishing bootstrap readiness.
Attaching hosts validate magic, layout version, host count, requested capacity,
stride, endpoint identity, and lock-free shared cursors. Initialization fails
rather than silently reducing capacity when the reserved queue region is too
small.

## Tests

`cxloom_queue_test` covers formatting, incompatible layouts, endpoint
permissions, full/empty backpressure, wrap-around, sequence ordering, and
100,000 concurrent messages. `cxloom_poller_test` uses two simultaneous producers to validate CPU binding, per-source FIFO, round-robin scanning, batch draining, idle backoff, statistics, and clean shutdown. `cxloom_shared_region_test` additionally sends
messages in both directions through two independently mapped views of one
shared backing file.

For the four-host devdax transport test, launch four containers and run:

    ./scripts/run-queue-transport-containers.sh

A longer or shorter run can be selected with:

    CL_QUEUE_ITERATIONS=1000000 CL_QUEUE_BATCH_SIZE=32 \
    ./scripts/run-queue-transport-containers.sh

## Measured platform result (2026-09-04)

Four processes pinned to NUMA nodes 0 through 3 mapped `/dev/dax0.0`. Every
host sent one message to and received one message from each of its three peers
per iteration, exercising all 12 directed rings concurrently.

- 1,000 iterations: zero errors on every host.
- 100,000 iterations: zero errors on every host.
- The original synchronous main-thread push/pop loop completed 600,000 operations per host in approximately 570 ms.
- The dedicated poller with batch size 32 completed the same operation count in approximately 313-314 ms, with zero errors.

This validates the transport on the current CPU-cache-coherent CXL emulation
platform. A physically non-coherent multi-host platform must rerun the test
with its measured visibility profile.
