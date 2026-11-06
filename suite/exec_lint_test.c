#include "suite/exec.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// Scratch lint path under /tmp.
static const char kScratch[] =
    "/tmp/exec_lint_case.yaml";

// Valid concord head.
static const char kCHead[] =
    "name: t\n"
    "version: 1\n"
    "sut: concord\n"
    "nodes: 2\n"
    "timeout_s: 10\n"
    "phases:\n";

// Valid weld head.
static const char kWHead[] =
    "name: t\n"
    "version: 1\n"
    "sut: weld\n"
    "nodes: 2\n"
    "domain: 64\n"
    "timeout_s: 10\n"
    "phases:\n";

// Mesh until phase (concord).
static const char kMesh[] =
    "  - name: mesh\n"
    "    until:\n"
    "      peers: 2\n"
    "      on: all\n"
    "    timeout_s: 10\n";

// Fixed-count until (any SUT).
static const char kMeshCount[] =
    "  - name: m\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Submit plus count (concord).
static const char kSubmit[] =
    "  - name: a\n"
    "    do:\n"
    "      - submit:\n"
    "          node: 0\n"
    "          image: probe\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Stop plus count (concord).
static const char kStopB[] =
    "  - name: b\n"
    "    do:\n"
    "      - stop:\n"
    "          workload: submitted\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Present workload (concord).
static const char kPresentB[] =
    "  - name: b\n"
    "    until:\n"
    "      present:\n"
    "        workload: submitted\n"
    "        on: all\n"
    "    timeout_s: 10\n";

// Absent workload (concord).
static const char kAbsentB[] =
    "  - name: b\n"
    "    until:\n"
    "      absent:\n"
    "        workload: submitted\n"
    "        on: all\n"
    "    timeout_s: 10\n";

// Assigned workload (concord).
static const char kAssignedB[] =
    "  - name: b\n"
    "    until:\n"
    "      assigned:\n"
    "        workload: submitted\n"
    "    timeout_s: 10\n";

// Until plus hold (concord).
static const char kHold[] =
    "  - name: mesh\n"
    "    until:\n"
    "      peers: 2\n"
    "      on: all\n"
    "    hold_s: 2\n"
    "    timeout_s: 10\n";

// Observed scalar on (weld).
static const char kObserved[] =
    "  - name: up\n"
    "    until:\n"
    "      observed:\n"
    "        on: 1\n"
    "        text: weld-a\n"
    "    timeout_s: 10\n";

// Observed lone-list on (weld).
static const char kObservedList[] =
    "  - name: up\n"
    "    until:\n"
    "      observed:\n"
    "        on:\n"
    "          - 1\n"
    "        text: weld-a\n"
    "    timeout_s: 10\n";

// Observed multi on (weld, bad).
static const char kObservedMulti[] =
    "  - name: up\n"
    "    until:\n"
    "      observed:\n"
    "        on:\n"
    "          - 0\n"
    "          - 1\n"
    "        text: weld-a\n"
    "    timeout_s: 10\n";

// Text present (weld-only).
static const char kTextPresent[] =
    "  - name: x\n"
    "    until:\n"
    "      present:\n"
    "        text: hi\n"
    "        on: all\n"
    "    timeout_s: 10\n";

// Pub step (weld-only).
static const char kPub[] =
    "  - name: x\n"
    "    do:\n"
    "      - pub:\n"
    "          from: 0\n"
    "          text: hi\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Unknown step verb.
static const char kBadVerb[] =
    "  - name: x\n"
    "    do:\n"
    "      - bogus: 1\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Unknown phase key.
static const char kBadPhaseKey[] =
    "  - name: mesh\n"
    "    bogus: 1\n"
    "    until:\n"
    "      peers: 2\n"
    "      on: all\n"
    "    timeout_s: 10\n";

// Empty drop list.
static const char kDropEmpty[] =
    "  - name: d\n"
    "    do:\n"
    "      - drop: []\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Nested empty item in drop.
static const char kDropNested[] =
    "  - name: d\n"
    "    do:\n"
    "      - drop:\n"
    "          - []\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Empty on list.
static const char kOnEmpty[] =
    "  - name: mesh\n"
    "    until:\n"
    "      peers: 2\n"
    "      on: []\n"
    "    timeout_s: 10\n";

// Zero peers count.
static const char kPeersZero[] =
    "  - name: mesh\n"
    "    until:\n"
    "      peers: 0\n"
    "      on: all\n"
    "    timeout_s: 10\n";

// Phase timeout over clamp.
static const char kTimeoutPhase[] =
    "  - name: mesh\n"
    "    until:\n"
    "      peers: 2\n"
    "      on: all\n"
    "    timeout_s: 3601\n";

// Huge phase timeout.
static const char kTimeoutHuge[] =
    "  - name: mesh\n"
    "    until:\n"
    "      peers: 2\n"
    "      on: all\n"
    "    timeout_s: 99999999999\n";

// Hold above timeout.
static const char kHoldAbove[] =
    "  - name: mesh\n"
    "    until:\n"
    "      peers: 2\n"
    "      on: all\n"
    "    hold_s: 5\n"
    "    timeout_s: 4\n";

// Hold over clamp.
static const char kHoldBig[] =
    "  - name: mesh\n"
    "    until:\n"
    "      peers: 2\n"
    "      on: all\n"
    "    hold_s: 3601\n"
    "    timeout_s: 10\n";

// Hold with sustain (bad).
static const char kHoldSustain[] =
    "  - name: m\n"
    "    sustain:\n"
    "      mesh_count: 1\n"
    "    hold_s: 1\n"
    "    timeout_s: 10\n";

// Two mesh phases share a name.
static const char kDup[] =
    "  - name: mesh\n"
    "    until:\n"
    "      peers: 2\n"
    "      on: all\n"
    "    timeout_s: 10\n"
    "  - name: mesh\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Concord head, unknown need.
static const char kCNeedBad[] =
    "name: t\n"
    "version: 1\n"
    "sut: concord\n"
    "nodes: 2\n"
    "needs:\n"
    "  - nope.frob\n"
    "timeout_s: 10\n"
    "phases:\n";

// Concord head, known need.
static const char kCNeedOk[] =
    "name: t\n"
    "version: 1\n"
    "sut: concord\n"
    "nodes: 2\n"
    "needs:\n"
    "  - workload.submit\n"
    "timeout_s: 10\n"
    "phases:\n";

// Concord head, zero timeout.
static const char kCTimeout0[] =
    "name: t\n"
    "version: 1\n"
    "sut: concord\n"
    "nodes: 2\n"
    "timeout_s: 0\n"
    "phases:\n";

// Concord head, timeout over clamp.
static const char kCTimeoutBig[] =
    "name: t\n"
    "version: 1\n"
    "sut: concord\n"
    "nodes: 2\n"
    "timeout_s: 3601\n"
    "phases:\n";

// Concord head, timeout at clamp.
static const char kCTimeoutMax[] =
    "name: t\n"
    "version: 1\n"
    "sut: concord\n"
    "nodes: 2\n"
    "timeout_s: 3600\n"
    "phases:\n";

// Concord head, bad version.
static const char kCVer2[] =
    "name: t\n"
    "version: 2\n"
    "sut: concord\n"
    "nodes: 2\n"
    "timeout_s: 10\n"
    "phases:\n";

// Concord head, quoted version.
static const char kCVerQuoted[] =
    "name: t\n"
    "version: \"1\"\n"
    "sut: concord\n"
    "nodes: 2\n"
    "timeout_s: 10\n"
    "phases:\n";

// Concord head, zero nodes.
static const char kCNodes0[] =
    "name: t\n"
    "version: 1\n"
    "sut: concord\n"
    "nodes: 0\n"
    "timeout_s: 10\n"
    "phases:\n";

// Concord head, too many nodes.
static const char kCNodes101[] =
    "name: t\n"
    "version: 1\n"
    "sut: concord\n"
    "nodes: 101\n"
    "timeout_s: 10\n"
    "phases:\n";

// Concord head, empty phases.
static const char kCPhasesEmpty[] =
    "name: t\n"
    "version: 1\n"
    "sut: concord\n"
    "nodes: 2\n"
    "timeout_s: 10\n"
    "phases: []\n";

// Concord head, weld-only domain.
static const char kCDomain[] =
    "name: t\n"
    "version: 1\n"
    "sut: concord\n"
    "nodes: 2\n"
    "domain: 64\n"
    "timeout_s: 10\n"
    "phases:\n";

// Concord head, weld-only peers.
static const char kCRos[] =
    "name: t\n"
    "version: 1\n"
    "sut: concord\n"
    "nodes: 2\n"
    "timeout_s: 10\n"
    "ros_peers:\n"
    "  - node: 0\n"
    "    distro: jazzy\n"
    "phases:\n";

// Weld head, concord-only spawn.
static const char kWSpawnNode[] =
    "name: t\n"
    "version: 1\n"
    "sut: weld\n"
    "nodes: 2\n"
    "domain: 64\n"
    "timeout_s: 10\n"
    "spawn:\n"
    "  node: 0\n"
    "phases:\n";

// Named submit (concord).
static const char kAsSubmit[] =
    "  - name: a\n"
    "    do:\n"
    "      - submit:\n"
    "          node: 0\n"
    "          image: probe\n"
    "          as: w0\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Second named submit, new name.
static const char kAsSubmit2[] =
    "  - name: b\n"
    "    do:\n"
    "      - submit:\n"
    "          node: 1\n"
    "          image: probe\n"
    "          as: w1\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Named present (concord).
static const char kNamedPresent[] =
    "  - name: b\n"
    "    until:\n"
    "      present:\n"
    "        workload: w0\n"
    "        on: all\n"
    "    timeout_s: 10\n";

// Counted submit (concord).
static const char kCountSubmit[] =
    "  - name: a\n"
    "    do:\n"
    "      - submit:\n"
    "          node: 0\n"
    "          image: probe\n"
    "          as: bulk\n"
    "          count: 3\n"
    "          rate: 2\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Base present (concord).
static const char kBasePresent[] =
    "  - name: b\n"
    "    until:\n"
    "      present:\n"
    "        workload: bulk\n"
    "        on: all\n"
    "    timeout_s: 10\n";

// Member present (concord).
static const char kMemberPresent[] =
    "  - name: b\n"
    "    until:\n"
    "      present:\n"
    "        workload: bulk-1\n"
    "        on: all\n"
    "    timeout_s: 10\n";

// Count without as (bad).
static const char kCountNoAs[] =
    "  - name: a\n"
    "    do:\n"
    "      - submit:\n"
    "          node: 0\n"
    "          image: probe\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Count zero (bad).
static const char kCountZero[] =
    "  - name: a\n"
    "    do:\n"
    "      - submit:\n"
    "          node: 0\n"
    "          image: probe\n"
    "          as: bulk\n"
    "          count: 0\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Count over clamp (bad).
static const char kCountBig[] =
    "  - name: a\n"
    "    do:\n"
    "      - submit:\n"
    "          node: 0\n"
    "          image: probe\n"
    "          as: bulk\n"
    "          count: 101\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Fixed-id submit (lint ok, run
// skips).
static const char kIdSubmit[] =
    "  - name: a\n"
    "    do:\n"
    "      - submit:\n"
    "          node: 0\n"
    "          image: probe\n"
    "          id: fixed-1\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Percent flags (lint ok).
static const char kPctSubmit[] =
    "  - name: a\n"
    "    do:\n"
    "      - submit:\n"
    "          node: 0\n"
    "          image: probe\n"
    "          flags: --memory "
    "60%host\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Bad workload name (bad).
static const char kBadAs[] =
    "  - name: a\n"
    "    do:\n"
    "      - submit:\n"
    "          node: 0\n"
    "          image: probe\n"
    "          as: \"bad name!\"\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Zero rate (bad).
static const char kRateZero[] =
    "  - name: a\n"
    "    do:\n"
    "      - submit:\n"
    "          node: 0\n"
    "          image: probe\n"
    "          as: bulk\n"
    "          count: 2\n"
    "          rate: 0\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Agree (concord).
static const char kAgree[] =
    "  - name: b\n"
    "    until:\n"
    "      agree:\n"
    "        workload: w0\n"
    "    timeout_s: 10\n";

// Assigned_on (concord).
static const char kAssignedOn[] =
    "  - name: b\n"
    "    until:\n"
    "      assigned_on:\n"
    "        workload: w0\n"
    "        node: 1\n"
    "    timeout_s: 10\n";

// Epoch gte (concord).
static const char kEpoch[] =
    "  - name: b\n"
    "    until:\n"
    "      epoch:\n"
    "        workload: w0\n"
    "        node: 0\n"
    "        gte: 1\n"
    "    timeout_s: 10\n";

// Epoch eq plus gte (bad).
static const char kEpochBoth[] =
    "  - name: b\n"
    "    until:\n"
    "      epoch:\n"
    "        workload: w0\n"
    "        node: 0\n"
    "        eq: 1\n"
    "        gte: 1\n"
    "    timeout_s: 10\n";

// Assigned_on missing node (bad).
static const char kAssignedOnNo[] =
    "  - name: b\n"
    "    until:\n"
    "      assigned_on:\n"
    "        workload: w0\n"
    "    timeout_s: 10\n";

// Unknown workload stop (bad).
static const char kStopGhost[] =
    "  - name: b\n"
    "    do:\n"
    "      - stop:\n"
    "          workload: ghost\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Named stop (concord).
static const char kStopW0[] =
    "  - name: b\n"
    "    do:\n"
    "      - stop:\n"
    "          workload: w0\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Defaulted present: no workload or
// text key means submitted.
static const char kPresentDefault[] =
    "  - name: b\n"
    "    until:\n"
    "      present:\n"
    "        on: all\n"
    "    timeout_s: 10\n";

// Update step (concord).
static const char kUpdate[] =
    "  - name: b\n"
    "    do:\n"
    "      - update:\n"
    "          workload: w0\n"
    "          image: probe:v2\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Image condition (concord).
static const char kImage[] =
    "  - name: c\n"
    "    until:\n"
    "      image:\n"
    "        workload: w0\n"
    "        value: probe:v2\n"
    "        on: all\n"
    "    timeout_s: 10\n";

// Bad update image (bad).
static const char kUpdateBad[] =
    "  - name: b\n"
    "    do:\n"
    "      - update:\n"
    "          workload: w0\n"
    "          image: \"bad!!\"\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Multicast off phase.
static const char kNomcast[] =
    "  - name: m\n"
    "    multicast: off\n"
    "    sustain:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Multicast bogus (bad).
static const char kNomcastBad[] =
    "  - name: m\n"
    "    multicast: sometimes\n"
    "    sustain:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Slow step (concord).
static const char kSlow[] =
    "  - name: d\n"
    "    do:\n"
    "      - slow:\n"
    "          nodes:\n"
    "            - 1\n"
    "          secs: 5\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Slow zero secs (bad).
static const char kSlowZero[] =
    "  - name: d\n"
    "    do:\n"
    "      - slow:\n"
    "          nodes: all\n"
    "          secs: 0\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Fault drop (concord).
static const char kFaultDrop[] =
    "  - name: d\n"
    "    do:\n"
    "      - drop:\n"
    "          nodes: all\n"
    "          loss: 0.2\n"
    "          delay_ms: 50\n"
    "          jitter_ms: 5\n"
    "          kbps: 1000\n"
    "          dup_pct: 1\n"
    "          dir: both\n"
    "    sustain:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Loss over one (bad).
static const char kLossBig[] =
    "  - name: d\n"
    "    do:\n"
    "      - drop:\n"
    "          nodes: all\n"
    "          loss: 2\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Bad dir (bad).
static const char kDirBad[] =
    "  - name: d\n"
    "    do:\n"
    "      - drop:\n"
    "          nodes: all\n"
    "          dir: sideways\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Bare directed drop in (ok,
// no fault keys).
static const char kDropDirIn[] =
    "  - name: d\n"
    "    do:\n"
    "      - drop:\n"
    "          nodes: all\n"
    "          dir: in\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Bare directed drop both (ok,
// no fault keys).
static const char kDropDirBoth[] =
    "  - name: d\n"
    "    do:\n"
    "      - drop:\n"
    "          nodes:\n"
    "            - 1\n"
    "          dir: both\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Non-string dir (bad).
static const char kDirNum[] =
    "  - name: d\n"
    "    do:\n"
    "      - drop:\n"
    "          nodes: all\n"
    "          dir: 1\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Dup over 100 (bad).
static const char kDupBig[] =
    "  - name: d\n"
    "    do:\n"
    "      - drop:\n"
    "          nodes: all\n"
    "          dup_pct: 101\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Drop mapping minus nodes (bad).
static const char kDropNoNodes[] =
    "  - name: d\n"
    "    do:\n"
    "      - drop:\n"
    "          loss: 0.2\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Rated pub (weld).
static const char kRatedPub[] =
    "  - name: x\n"
    "    do:\n"
    "      - pub:\n"
    "          from: 0\n"
    "          text: hi\n"
    "          rate: 10\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Weld benchmark, count form.
static const char kBenchWeld[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: 1\n"
    "          rate: 5\n"
    "          count: 3\n"
    "          warmup: 1\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Weld benchmark, duration
// only, no warmup key.
static const char kBenchWeldDur[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: 1\n"
    "          rate: 5\n"
    "          duration_s: 2\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Concord benchmark, count
// form with a base name.
static const char kBenchConc[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          node: 0\n"
    "          image: probe\n"
    "          as: bench0\n"
    "          rate: 2\n"
    "          count: 3\n"
    "          warmup: 1\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 30\n";

// Concord benchmark, duration
// only form.
static const char kBenchConcDur[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          node: 0\n"
    "          image: probe\n"
    "          as: benchd\n"
    "          rate: 2\n"
    "          duration_s: 2\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 30\n";

// Benchmark minus any bound
// (bad: needs count or time).
static const char kBenchNoBound[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: 1\n"
    "          rate: 5\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Benchmark minus rate (bad).
static const char kBenchNoRate[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: 1\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Benchmark rate zero (bad).
static const char kBenchRate0[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: 1\n"
    "          rate: 0\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Benchmark count zero (bad).
static const char kBenchCount0[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: 1\n"
    "          rate: 5\n"
    "          count: 0\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Weld count over 1000 (bad).
static const char kBenchCountBig[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: 1\n"
    "          rate: 5\n"
    "          count: 1001\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Concord count over 100
// (bad: each issue is a row).
static const char kBenchConcBig[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          node: 0\n"
    "          image: probe\n"
    "          as: bench0\n"
    "          rate: 2\n"
    "          count: 101\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 30\n";

// Warmup at count (bad: must
// sit below it).
static const char kBenchWarm[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: 1\n"
    "          rate: 5\n"
    "          count: 3\n"
    "          warmup: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Duration over clamp (bad).
static const char kBenchDurBig[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: 1\n"
    "          rate: 5\n"
    "          duration_s: 3601\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Rate times duration over the
// concord cap (bad: too many).
static const char kBenchTooLarge[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          node: 0\n"
    "          image: probe\n"
    "          as: bench0\n"
    "          rate: 100\n"
    "          duration_s: 2\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 30\n";

// Weld shape with image (bad:
// concord-only key).
static const char kBenchWeldImg[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: 1\n"
    "          image: probe\n"
    "          rate: 5\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Concord shape with text
// (bad: weld-only key).
static const char kBenchConcText[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          node: 0\n"
    "          image: probe\n"
    "          as: bench0\n"
    "          text: load\n"
    "          rate: 2\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 30\n";

// Concord minus as (bad: base
// names the minted set).
static const char kBenchConcNoAs[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          node: 0\n"
    "          image: probe\n"
    "          rate: 2\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 30\n";

// Weld minus on (bad).
static const char kBenchWeldNoOn[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          rate: 5\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Weld shape on concord (bad:
// from is weld-only).
static const char kBenchWeldOnC[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: 1\n"
    "          rate: 5\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 30\n";

// Concord shape on weld (bad:
// node is concord-only).
static const char kBenchConcOnW[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          node: 0\n"
    "          image: probe\n"
    "          as: bench0\n"
    "          rate: 2\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Unknown benchmark key (bad).
static const char kBenchBogus[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: 1\n"
    "          rate: 5\n"
    "          count: 3\n"
    "          bogus: 1\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Replay (weld).
static const char kReplay[] =
    "  - name: x\n"
    "    do:\n"
    "      - replay:\n"
    "          file: msgs.log\n"
    "          topic: /chatter\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Replay minus file (bad).
static const char kReplayNoFile[] =
    "  - name: x\n"
    "    do:\n"
    "      - replay:\n"
    "          topic: /chatter\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Diskfill (concord).
static const char kDiskfill[] =
    "  - name: d\n"
    "    do:\n"
    "      - diskfill:\n"
    "          node: 0\n"
    "          mb: 16\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Diskfill over cap (bad).
static const char kDiskfillBig[] =
    "  - name: d\n"
    "    do:\n"
    "      - diskfill:\n"
    "          node: 0\n"
    "          mb: 65537\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Readonly (concord).
static const char kReadonly[] =
    "  - name: d\n"
    "    do:\n"
    "      - diskreadonly:\n"
    "          node: 0\n"
    "          state: on\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Readonly bad state (bad).
static const char kReadonlyBad[] =
    "  - name: d\n"
    "    do:\n"
    "      - diskreadonly:\n"
    "          node: 0\n"
    "          state: maybe\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Journal corrupt (concord).
static const char kJcorrupt[] =
    "  - name: d\n"
    "    do:\n"
    "      - journalcorrupt:\n"
    "          node: 1\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Journal corrupt off range (bad).
static const char kJcorruptBad[] =
    "  - name: d\n"
    "    do:\n"
    "      - journalcorrupt:\n"
    "          node: 9\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Push with all keys (concord).
static const char kPush[] =
    "  - name: d\n"
    "    do:\n"
    "      - push:\n"
    "          repo: concord-hog\n"
    "          tag: latest\n"
    "          rootfs: img/rootfs\n"
    "          entrypoint: /hog\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 10\n";

// Push defaults (concord).
static const char kPushBare[] =
    "  - name: d\n"
    "    do:\n"
    "      - push:\n"
    "          repo: concord-test\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 10\n";

// Push on weld (bad).
static const char kPushWeld[] =
    "  - name: d\n"
    "    do:\n"
    "      - push:\n"
    "          repo: concord-hog\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Push bad tag (bad).
static const char kPushBad[] =
    "  - name: d\n"
    "    do:\n"
    "      - push:\n"
    "          repo: concord-hog\n"
    "          tag: has space\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 10\n";

// Concord head with bins.
static const char kCBins[] =
    "name: t\n"
    "version: 1\n"
    "sut: concord\n"
    "nodes: 2\n"
    "timeout_s: 10\n"
    "bins:\n"
    "  1: ./bin/concord\n"
    "phases:\n";

// Concord head, bins off range.
static const char kCBinsBad[] =
    "name: t\n"
    "version: 1\n"
    "sut: concord\n"
    "nodes: 2\n"
    "timeout_s: 10\n"
    "bins:\n"
    "  5: ./bin/concord\n"
    "phases:\n";

// Concord head, bins empty path.
static const char kCBinsEmpty[] =
    "name: t\n"
    "version: 1\n"
    "sut: concord\n"
    "nodes: 2\n"
    "timeout_s: 10\n"
    "bins:\n"
    "  1: \"\"\n"
    "phases:\n";

// Concord head with disk cap.
static const char kCDiskCap[] =
    "name: t\n"
    "version: 1\n"
    "sut: concord\n"
    "nodes: 2\n"
    "timeout_s: 10\n"
    "disk_cap_mb: 512\n"
    "phases:\n";

// Concord head, zero disk cap
// (explicit uncapped).
static const char kCDiskCap0[] =
    "name: t\n"
    "version: 1\n"
    "sut: concord\n"
    "nodes: 2\n"
    "timeout_s: 10\n"
    "disk_cap_mb: 0\n"
    "phases:\n";

// Concord head, disk cap over
// bound.
static const char kCDiskCapBig[] =
    "name: t\n"
    "version: 1\n"
    "sut: concord\n"
    "nodes: 2\n"
    "timeout_s: 10\n"
    "disk_cap_mb: 65537\n"
    "phases:\n";

// Concord head, negative disk
// cap.
static const char kCDiskCapNeg[] =
    "name: t\n"
    "version: 1\n"
    "sut: concord\n"
    "nodes: 2\n"
    "timeout_s: 10\n"
    "disk_cap_mb: -1\n"
    "phases:\n";

// Weld head with disk cap (bad,
// concord-only).
static const char kWDiskCap[] =
    "name: t\n"
    "version: 1\n"
    "sut: weld\n"
    "nodes: 2\n"
    "domain: 64\n"
    "timeout_s: 10\n"
    "disk_cap_mb: 512\n"
    "phases:\n";

// Concord head, skew spawn.
static const char kCSkew[] =
    "name: t\n"
    "version: 1\n"
    "sut: concord\n"
    "nodes: 2\n"
    "timeout_s: 10\n"
    "spawn:\n"
    "  variant: skew\n"
    "  node: 1\n"
    "  offset: \"300\"\n"
    "phases:\n";

// Weld head with relay need.
static const char kWNeedRelay[] =
    "name: t\n"
    "version: 1\n"
    "sut: weld\n"
    "nodes: 2\n"
    "domain: 64\n"
    "needs:\n"
    "  - relay.echo\n"
    "timeout_s: 10\n"
    "phases:\n";

// Weld head with ros need.
static const char kWNeedRos[] =
    "name: t\n"
    "version: 1\n"
    "sut: weld\n"
    "nodes: 2\n"
    "domain: 64\n"
    "needs:\n"
    "  - ros.pub\n"
    "timeout_s: 10\n"
    "phases:\n";

// Weld benchmark with echo.
static const char kBenchEcho[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: 0\n"
    "          rate: 5\n"
    "          count: 3\n"
    "          warmup: 1\n"
    "          echo:\n"
    "            by: weld\n"
    "            node: 1\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Echo with bad by (bad).
static const char kBenchEchoBad[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: 0\n"
    "          rate: 5\n"
    "          count: 3\n"
    "          echo:\n"
    "            by: foo\n"
    "            node: 1\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Echo node out of range (bad).
static const char kBenchEchoNode[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: 0\n"
    "          rate: 5\n"
    "          count: 3\n"
    "          echo:\n"
    "            by: weld\n"
    "            node: 9\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Weld benchmark from ros:0.
static const char kBenchRos[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: ros:0\n"
    "          text: load\n"
    "          on: 1\n"
    "          rate: 2\n"
    "          count: 3\n"
    "          warmup: 1\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// From ros bad index (bad).
static const char kBenchRosBad[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: ros:9\n"
    "          text: load\n"
    "          on: 1\n"
    "          rate: 2\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Weld benchmark with payload.
static const char kBenchPay[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          on: 1\n"
    "          rate: 5\n"
    "          count: 3\n"
    "          warmup: 1\n"
    "          payload_bytes: 64\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Payload over cap (bad).
static const char kBenchPayBig[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: 1\n"
    "          rate: 5\n"
    "          count: 3\n"
    "          payload_bytes: 65537\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Old bench tag text (bad).
static const char kBenchOldTag[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: bench#3# hi\n"
    "          on: 1\n"
    "          rate: 5\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Ramp key in benchmark (bad).
static const char kBenchRamp[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: 1\n"
    "          rate: 5\n"
    "          count: 3\n"
    "          ramp_up: 2\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Weld alias pair (ok).
static const char kBenchAlias[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: weld:0\n"
    "          text: load\n"
    "          on: weld:1\n"
    "          rate: 5\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Weld to ros pair (ok).
static const char kBenchW2R[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: ros:1\n"
    "          rate: 5\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// From concord (bad, no pub).
static const char kBenchFromC[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: concord:0\n"
    "          text: load\n"
    "          on: 1\n"
    "          rate: 5\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// On concord (bad, no log).
static const char kBenchOnC[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: concord:1\n"
    "          rate: 5\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Echo by concord (bad).
static const char kBenchEchoC[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: 0\n"
    "          rate: 5\n"
    "          count: 3\n"
    "          echo:\n"
    "            by: concord\n"
    "            node: 1\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Echo relay ros (bad).
static const char kBenchRelayR[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: 0\n"
    "          rate: 5\n"
    "          count: 3\n"
    "          echo:\n"
    "            by: weld\n"
    "            node: ros:1\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Valid ramp (ok).
static const char kRampOk[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: 1\n"
    "          ramp:\n"
    "            start_rate: 5\n"
    "            step_rate: 5\n"
    "            step_duration_s: 2\n"
    "            max_rate: 20\n"
    "            max_steps: 4\n"
    "            max_duration_s: 30\n"
    "            loss_pct: 5\n"
    "            p99_ms: 100\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Ramp minus stop (bad).
static const char kRampNoStop[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: 1\n"
    "          ramp:\n"
    "            start_rate: 5\n"
    "            step_rate: 5\n"
    "            step_duration_s: 2\n"
    "            max_rate: 20\n"
    "            max_steps: 4\n"
    "            max_duration_s: 30\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Ramp bad start (bad).
static const char kRampBadStart[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: 1\n"
    "          ramp:\n"
    "            start_rate: 0\n"
    "            step_rate: 5\n"
    "            step_duration_s: 2\n"
    "            max_rate: 20\n"
    "            max_steps: 4\n"
    "            max_duration_s: 30\n"
    "            loss_pct: 5\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Ramp bad step (bad).
static const char kRampBadStep[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: 1\n"
    "          ramp:\n"
    "            start_rate: 5\n"
    "            step_rate: 0\n"
    "            step_duration_s: 2\n"
    "            max_rate: 20\n"
    "            max_steps: 4\n"
    "            max_duration_s: 30\n"
    "            loss_pct: 5\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Ramp with rate (bad).
static const char kRampWithRate[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: 1\n"
    "          rate: 5\n"
    "          ramp:\n"
    "            start_rate: 5\n"
    "            step_rate: 5\n"
    "            step_duration_s: 2\n"
    "            max_rate: 20\n"
    "            max_steps: 4\n"
    "            max_duration_s: 30\n"
    "            loss_pct: 5\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Ramp step too large (bad).
static const char kRampLarge[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: load\n"
    "          on: 1\n"
    "          ramp:\n"
    "            start_rate: 500\n"
    "            step_rate: 500\n"
    "            step_duration_s: 2\n"
    "            max_rate: 1000\n"
    "            max_steps: 4\n"
    "            max_duration_s: 30\n"
    "            loss_pct: 5\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Payload at new cap (ok).
static const char kBenchPayMax[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          on: 1\n"
    "          rate: 5\n"
    "          count: 3\n"
    "          payload_bytes: 65536\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Ros pub step (weld).
static const char kRosPub[] =
    "  - name: x\n"
    "    do:\n"
    "      - ros_pub:\n"
    "          from: 0\n"
    "          topic: /chatter\n"
    "          text: hi\n"
    "          rate: 2\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Ros pub minus rate (bad).
static const char kRosPubNoRate[] =
    "  - name: x\n"
    "    do:\n"
    "      - ros_pub:\n"
    "          from: 0\n"
    "          topic: /chatter\n"
    "          text: hi\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// Ros pub on concord (bad).
static const char kRosPubConc[] =
    "  - name: x\n"
    "    do:\n"
    "      - ros_pub:\n"
    "          from: 0\n"
    "          topic: /chatter\n"
    "          text: hi\n"
    "          rate: 2\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 10\n";

// New style tag text (ok, prefix
// anchored, not old two-field).
static const char kBenchNewTag[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          from: 0\n"
    "          text: bench#1#2#hi\n"
    "          on: 1\n"
    "          rate: 5\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Generic weld to weld (ok).
static const char kGenW2W[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          send:\n"
    "            node: 0\n"
    "            via: weld-pub\n"
    "            body: load\n"
    "          expect:\n"
    "            node: 1\n"
    "            via: log-contains\n"
    "            match: load\n"
    "          rate: 5\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Generic scrape gate (ok).
static const char kGenScrape[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          send:\n"
    "            node: 0\n"
    "            via: weld-pub\n"
    "            body: load\n"
    "          expect:\n"
    "            node: 1\n"
    "            via: prometheus-scrape\n"
    "            match:\n"
    "              metric: weld_mismatch_total\n"
    "              op: le\n"
    "              value: 0\n"
    "              port: 9100\n"
    "          rate: 5\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Generic scrape missing port (bad).
static const char kGenScrapeNoPort[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          send:\n"
    "            node: 0\n"
    "            via: weld-pub\n"
    "            body: load\n"
    "          expect:\n"
    "            node: 1\n"
    "            via: prometheus-scrape\n"
    "            match:\n"
    "              metric: weld_mismatch_total\n"
    "              op: le\n"
    "              value: 0\n"
    "          rate: 5\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Generic scrape bad op (bad).
static const char kGenScrapeBadOp[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          send:\n"
    "            node: 0\n"
    "            via: weld-pub\n"
    "            body: load\n"
    "          expect:\n"
    "            node: 1\n"
    "            via: prometheus-scrape\n"
    "            match:\n"
    "              metric: weld_mismatch_total\n"
    "              op: near\n"
    "              value: 0\n"
    "              port: 9100\n"
    "          rate: 5\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Weld head with a rolling peer on node 0.
static const char kWRoll[] =
    "name: t\n"
    "version: 1\n"
    "sut: weld\n"
    "nodes: 2\n"
    "domain: 64\n"
    "needs:\n"
    "  - ros.pub\n"
    "  - ros.peer\n"
    "timeout_s: 10\n"
    "ros_peers:\n"
    "  - node: 0\n"
    "    distro: rolling\n"
    "phases:\n";

// Generic rolling drive (ok): publish through the node 0
// rolling peer, observe on the weld log.
static const char kGenRoll[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          send:\n"
    "            node: 0\n"
    "            via: ros-pub\n"
    "            body: load\n"
    "          expect:\n"
    "            node: 1\n"
    "            via: log-contains\n"
    "            match: load\n"
    "          rate: 5\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Generic weld to ros (ok).
static const char kGenW2R[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          send:\n"
    "            node: 0\n"
    "            via: weld-pub\n"
    "            body: load\n"
    "          expect:\n"
    "            node: 1\n"
    "            via: ros-log-contains\n"
    "            match: load\n"
    "          rate: 5\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Generic ros to ros (ok).
static const char kGenR2R[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          send:\n"
    "            node: 0\n"
    "            via: ros-pub\n"
    "            body: load\n"
    "          expect:\n"
    "            node: 1\n"
    "            via: ros-log-contains\n"
    "            match: load\n"
    "          rate: 2\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Generic concord run (ok).
static const char kGenConc[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          send:\n"
    "            node: 0\n"
    "            via: workload-submit\n"
    "            image: probe\n"
    "            as: bench0\n"
    "          expect:\n"
    "            node: 0\n"
    "            via: workload-running\n"
    "            match: running\n"
    "          rate: 2\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 30\n";

// Generic journal map (ok).
static const char kGenJour[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          send:\n"
    "            node: 0\n"
    "            via: workload-submit\n"
    "            image: probe\n"
    "            as: bench0\n"
    "          expect:\n"
    "            node: 0\n"
    "            via: journal-has\n"
    "            match:\n"
    "              field: Image\n"
    "              value: probe\n"
    "          rate: 2\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 30\n";

// Generic payload tpl (ok).
static const char kGenTpl[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          send:\n"
    "            node: 0\n"
    "            via: weld-pub\n"
    "            body: load\n"
    "            payload: \"bench#"
    "{{seq}}#{{send_ns}}#"
    "{{body}}\"\n"
    "          expect:\n"
    "            node: 1\n"
    "            via: log-contains\n"
    "            match: load\n"
    "          rate: 5\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Generic nonsense pair (ok,
// 100 percent loss, never lint).
static const char kGenNon[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          send:\n"
    "            node: 0\n"
    "            via: weld-pub\n"
    "            body: load\n"
    "          expect:\n"
    "            node: 0\n"
    "            via: workload-running\n"
    "            match: running\n"
    "          rate: 5\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Generic bad via (bad).
static const char kGenBadVia[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          send:\n"
    "            node: 0\n"
    "            via: bogus-pub\n"
    "            body: load\n"
    "          expect:\n"
    "            node: 1\n"
    "            via: log-contains\n"
    "            match: load\n"
    "          rate: 5\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Generic bad expect via (bad).
static const char kGenBadExp[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          send:\n"
    "            node: 0\n"
    "            via: weld-pub\n"
    "            body: load\n"
    "          expect:\n"
    "            node: 1\n"
    "            via: bogus-seen\n"
    "            match: load\n"
    "          rate: 5\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Generic missing body (bad).
static const char kGenNoBody[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          send:\n"
    "            node: 0\n"
    "            via: weld-pub\n"
    "          expect:\n"
    "            node: 1\n"
    "            via: log-contains\n"
    "            match: load\n"
    "          rate: 5\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Generic missing image (bad).
static const char kGenNoImg[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          send:\n"
    "            node: 0\n"
    "            via: workload-submit\n"
    "            as: bench0\n"
    "          expect:\n"
    "            node: 0\n"
    "            via: workload-running\n"
    "            match: running\n"
    "          rate: 2\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 30\n";

// Generic missing match (bad).
static const char kGenNoMatch[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          send:\n"
    "            node: 0\n"
    "            via: weld-pub\n"
    "            body: load\n"
    "          expect:\n"
    "            node: 1\n"
    "            via: log-contains\n"
    "          rate: 5\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Generic bad placeholder (bad).
static const char kGenBadPh[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          send:\n"
    "            node: 0\n"
    "            via: weld-pub\n"
    "            body: load\n"
    "            payload: \"bench#"
    "{{oops}}\"\n"
    "          expect:\n"
    "            node: 1\n"
    "            via: log-contains\n"
    "            match: load\n"
    "          rate: 5\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Generic wid outside (bad).
static const char kGenWidBad[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          send:\n"
    "            node: 0\n"
    "            via: weld-pub\n"
    "            body: load\n"
    "            payload: \"bench#"
    "{{wid}}\"\n"
    "          expect:\n"
    "            node: 1\n"
    "            via: log-contains\n"
    "            match: load\n"
    "          rate: 5\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Generic bad state (bad).
static const char kGenBadState[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          send:\n"
    "            node: 0\n"
    "            via: workload-submit\n"
    "            image: probe\n"
    "            as: bench0\n"
    "          expect:\n"
    "            node: 0\n"
    "            via: workload-running\n"
    "            match: stopped\n"
    "          rate: 2\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 30\n";

// Generic bad field (bad).
static const char kGenBadField[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          send:\n"
    "            node: 0\n"
    "            via: workload-submit\n"
    "            image: probe\n"
    "            as: bench0\n"
    "          expect:\n"
    "            node: 0\n"
    "            via: journal-has\n"
    "            match:\n"
    "              field: Bogus\n"
    "              value: probe\n"
    "          rate: 2\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 1\n"
    "    timeout_s: 30\n";

// Generic send-only key (bad).
static const char kGenSendOnly[] =
    "  - name: bench\n"
    "    do:\n"
    "      - benchmark:\n"
    "          send:\n"
    "            node: 0\n"
    "            via: weld-pub\n"
    "            bogus: 1\n"
    "            body: load\n"
    "          expect:\n"
    "            node: 1\n"
    "            via: log-contains\n"
    "            match: load\n"
    "          rate: 5\n"
    "          count: 3\n"
    "    until:\n"
    "      mesh_count: 2\n"
    "    timeout_s: 30\n";

// Writes text to scratch. Asserts ok.
static void scratch(const char *text) {
    FILE *fp = fopen(kScratch, "w");

    assert(fp != nullptr);
    assert(fputs(text, fp) >= 0);
    assert(fclose(fp) == 0);
}

// Lints head plus phases. Asserts pass.
static void doc_ok(const char *head,
                   const char *p1,
                   const char *p2) {
    char buf[2048];
    char why[512];
    bool good;
    int m;

    m = snprintf(buf, sizeof(buf),
                 "%s%s%s", head, p1,
                 p2);
    assert(m > 0 &&
           m < (int)sizeof(buf));
    scratch(buf);
    why[0] = '\0';
    good = suite_lint_file(kScratch,
                           why,
                           sizeof(why));
    assert(good);
}

// Lints head plus phases. Asserts a
// file:line failure.
static void doc_bad(const char *head,
                    const char *p1,
                    const char *p2) {
    char buf[2048];
    char why[512];
    bool good;
    int m;

    m = snprintf(buf, sizeof(buf),
                 "%s%s%s", head, p1,
                 p2);
    assert(m > 0 &&
           m < (int)sizeof(buf));
    scratch(buf);
    why[0] = '\0';
    good = suite_lint_file(kScratch,
                           why,
                           sizeof(why));
    assert(!good);
    assert(why[0] != '\0');
    assert(strstr(why, ".yaml:") !=
           nullptr);
}

// Runs lint checks. Returns 0.
int main(void) {
    doc_ok(kCHead, kMesh, "");
    doc_ok(kWHead, kMeshCount, "");
    doc_ok(kWHead, kObserved, "");
    doc_ok(kWHead, kObservedList,
           "");
    doc_ok(kCHead, kHold, "");
    doc_ok(kCHead, kSubmit, kStopB);
    doc_ok(kCHead, kSubmit,
           kPresentB);
    doc_ok(kCHead, kAsSubmit,
           kNamedPresent);
    doc_ok(kCHead, kAsSubmit,
           kAsSubmit2);
    doc_ok(kCHead, kAsSubmit,
           kStopW0);
    doc_ok(kCHead, kCountSubmit,
           kBasePresent);
    doc_ok(kCHead, kCountSubmit,
           kMemberPresent);
    doc_ok(kCHead, kSubmit,
           kPresentDefault);
    doc_ok(kCHead, kIdSubmit,
           kMeshCount);
    doc_ok(kCHead, kPctSubmit,
           kMeshCount);
    doc_ok(kCHead, kAsSubmit,
           kAgree);
    doc_ok(kCHead, kAsSubmit,
           kAssignedOn);
    doc_ok(kCHead, kAsSubmit,
           kEpoch);
    doc_ok(kCHead, kAsSubmit,
           kUpdate);
    doc_ok(kCHead, kAsSubmit,
           kImage);
    doc_ok(kCHead, kNomcast, "");
    doc_ok(kCHead, kSlow,
           kMeshCount);
    doc_ok(kCHead, kFaultDrop, "");
    doc_ok(kCHead, kDropDirIn, "");
    doc_ok(kCHead, kDropDirBoth,
           "");
    doc_ok(kWHead, kRatedPub, "");
    doc_ok(kWHead, kBenchWeld, "");
    doc_ok(kWHead, kBenchWeldDur,
           "");
    doc_ok(kCHead, kBenchConc, "");
    doc_ok(kCHead, kBenchConcDur,
           "");
    doc_ok(kWHead, kReplay, "");
    doc_ok(kCHead, kDiskfill,
           kMeshCount);
    doc_ok(kCHead, kReadonly,
           kMeshCount);
    doc_ok(kCHead, kJcorrupt,
           kMeshCount);
    doc_ok(kCHead, kPush,
           kMeshCount);
    doc_ok(kCHead, kPushBare,
           kMeshCount);
    doc_ok(kCBins, kMesh, "");
    doc_ok(kCDiskCap, kMesh, "");
    doc_ok(kCDiskCap0, kMesh, "");
    doc_ok(kCSkew, kMesh, "");
    doc_ok(kCNeedOk, kMesh, "");
    doc_ok(kCVerQuoted, kMesh, "");
    doc_ok(kCTimeoutMax, kMesh, "");
    doc_ok(kWNeedRelay, kBenchEcho,
           "");
    doc_ok(kWNeedRelay, kMeshCount,
           "");
    doc_ok(kWNeedRos, kBenchRos,
           "");
    doc_ok(kWNeedRos, kRosPub, "");
    doc_ok(kWHead, kBenchPay, "");
    doc_ok(kWNeedRos, kMeshCount,
           "");
    doc_ok(kWHead, kBenchNewTag,
           "");
    doc_ok(kWHead, kBenchAlias,
           "");
    doc_ok(kWHead, kBenchW2R,
           "");
    doc_ok(kWHead, kRampOk,
           "");
    doc_ok(kWHead, kBenchPayMax,
           "");

    doc_bad(kCHead, kMesh,
            "bogus: 1\n");
    doc_bad(kCHead, kBadPhaseKey,
            "");
    doc_bad(kCHead, kBadVerb, "");
    doc_bad(kCHead, kStopB, "");
    doc_bad(kCHead, kPresentB, "");
    doc_bad(kCHead, kAbsentB, "");
    doc_bad(kCHead, kAssignedB,
            "");
    doc_bad(kCHead, kDup, "");
    doc_bad(kCNeedBad, kMesh, "");
    doc_bad(kWHead, kSubmit, "");
    doc_bad(kCHead, kPub, "");
    doc_bad(kWHead, kMesh, "");
    doc_bad(kCHead, kObserved, "");
    doc_bad(kCHead, kTextPresent,
            "");
    doc_bad(kWHead, kPresentB, "");
    doc_bad(kCTimeout0, kMesh, "");
    doc_bad(kCTimeoutBig, kMesh,
            "");
    doc_bad(kCHead, kTimeoutPhase,
            "");
    doc_bad(kCHead, kTimeoutHuge,
            "");
    doc_bad(kCHead, kHoldAbove, "");
    doc_bad(kCHead, kHoldBig, "");
    doc_bad(kCHead, kHoldSustain,
            "");
    doc_bad(kCNodes0, kMesh, "");
    doc_bad(kCNodes101, kMesh, "");
    doc_bad(kCHead, kPeersZero, "");
    doc_bad(kCPhasesEmpty, "", "");
    doc_bad(kCHead, kOnEmpty, "");
    doc_bad(kCHead, kDropEmpty, "");
    doc_bad(kCHead, kDropNested,
            "");
    doc_bad(kWHead, kObservedMulti,
            "");
    doc_bad(kCVer2, kMesh, "");
    doc_bad(kCHead, kAsSubmit,
            kSubmit);
    doc_bad(kCHead, kAsSubmit,
            kAsSubmit);
    doc_bad(kCHead, kCountNoAs,
            "");
    doc_bad(kCHead, kCountZero,
            "");
    doc_bad(kCHead, kCountBig,
            "");
    doc_bad(kCHead, kBadAs, "");
    doc_bad(kCHead, kRateZero,
            "");
    doc_bad(kWHead, kBenchNoBound,
            "");
    doc_bad(kWHead, kBenchNoRate,
            "");
    doc_bad(kWHead, kBenchRate0,
            "");
    doc_bad(kWHead, kBenchCount0,
            "");
    doc_bad(kWHead, kBenchCountBig,
            "");
    doc_bad(kCHead, kBenchConcBig,
            "");
    doc_bad(kWHead, kBenchWarm, "");
    doc_bad(kWHead, kBenchDurBig,
            "");
    doc_bad(kCHead, kBenchTooLarge,
            "");
    doc_bad(kWHead, kBenchWeldImg,
            "");
    doc_bad(kCHead, kBenchConcText,
            "");
    doc_bad(kCHead, kBenchConcNoAs,
            "");
    doc_bad(kWHead, kBenchWeldNoOn,
            "");
    doc_bad(kCHead, kBenchWeldOnC,
            "");
    doc_bad(kWHead, kBenchConcOnW,
            "");
    doc_bad(kWHead, kBenchBogus,
            "");
    doc_bad(kCHead, kStopGhost,
            "");
    doc_bad(kCHead, kAsSubmit,
            kStopB);
    doc_bad(kCHead, kAgree, "");
    doc_bad(kCHead, kEpoch, "");
    doc_bad(kCHead, kUpdate,
            kMeshCount);
    doc_bad(kCHead, kEpochBoth,
            "");
    doc_bad(kCHead, kAssignedOnNo,
            "");
    doc_bad(kWHead, kAgree, "");
    doc_bad(kWHead, kEpoch, "");
    doc_bad(kCHead, kUpdateBad,
            "");
    doc_bad(kWHead, kUpdate, "");
    doc_bad(kWHead, kImage, "");
    doc_bad(kCHead, kNomcastBad,
            "");
    doc_bad(kCHead, kSlowZero,
            "");
    doc_bad(kCHead, kLossBig,
            "");
    doc_bad(kCHead, kDirBad, "");
    doc_bad(kCHead, kDirNum, "");
    doc_bad(kCHead, kDupBig, "");
    doc_bad(kCHead, kDropNoNodes,
            "");
    doc_bad(kWHead, kReplayNoFile,
            "");
    doc_bad(kCHead, kReplay, "");
    doc_bad(kWHead, kDiskfill, "");
    doc_bad(kCHead, kDiskfillBig,
            "");
    doc_bad(kWHead, kReadonly, "");
    doc_bad(kCHead, kReadonlyBad,
            "");
    doc_bad(kWHead, kJcorrupt, "");
    doc_bad(kCHead, kJcorruptBad,
            "");
    doc_bad(kWHead, kPushWeld, "");
    doc_bad(kCHead, kPushBad,
            "");
    doc_bad(kCBinsBad, kMesh, "");
    doc_bad(kCBinsEmpty, kMesh,
            "");
    doc_bad(kCDiskCapBig, kMesh,
            "");
    doc_bad(kCDiskCapNeg, kMesh,
            "");
    doc_bad(kWDiskCap, kMeshCount,
            "");
    doc_bad(kCHead, kSubmit,
            kSubmit);
    doc_bad(kCDomain, kMesh, "");
    doc_bad(kCRos, kMesh, "");
    doc_bad(kWSpawnNode, kMeshCount,
            "");
    doc_bad(kWHead, kBenchEcho, "");
    doc_bad(kWNeedRelay,
            kBenchEchoBad, "");
    doc_bad(kWNeedRelay,
            kBenchEchoNode, "");
    doc_bad(kCHead, kBenchEcho, "");
    doc_bad(kWHead, kBenchRos, "");
    doc_bad(kWNeedRos, kBenchRosBad,
            "");
    doc_bad(kWHead, kBenchPayBig,
            "");
    doc_bad(kCHead, kBenchPay, "");
    doc_bad(kWHead, kBenchOldTag,
            "");
    doc_bad(kWHead, kBenchRamp, "");
    doc_bad(kCHead, kBenchRamp, "");
    doc_bad(kWHead, kRosPubNoRate,
            "");
    doc_bad(kCHead, kRosPubConc,
            "");
    doc_bad(kWHead, kRosPub, "");
    doc_bad(kWHead, kBenchFromC,
            "");
    doc_bad(kWHead, kBenchOnC,
            "");
    doc_bad(kWNeedRelay, kBenchEchoC,
            "");
    doc_bad(kWNeedRelay, kBenchRelayR,
            "");
    doc_bad(kWHead, kRampNoStop,
            "");
    doc_bad(kWHead, kRampBadStart,
            "");
    doc_bad(kWHead, kRampBadStep,
            "");
    doc_bad(kWHead, kRampWithRate,
            "");
    doc_bad(kWHead, kRampLarge,
            "");
    doc_ok(kWHead, kGenW2W, "");
    doc_ok(kWHead, kGenScrape, "");
    doc_ok(kWRoll, kGenRoll, "");
    doc_bad(kWHead, kGenScrapeNoPort, "");
    doc_bad(kWHead, kGenScrapeBadOp, "");
    doc_ok(kWHead, kGenW2R, "");
    doc_ok(kWNeedRos, kGenR2R, "");
    doc_ok(kCHead, kGenConc, "");
    doc_ok(kCHead, kGenJour, "");
    doc_ok(kWHead, kGenTpl, "");
    doc_ok(kWHead, kGenNon, "");
    doc_ok(kWHead, kBenchWeld, "");
    doc_bad(kWHead, kGenBadVia, "");
    doc_bad(kWHead, kGenBadExp, "");
    doc_bad(kWHead, kGenNoBody, "");
    doc_bad(kCHead, kGenNoImg, "");
    doc_bad(kWHead, kGenNoMatch,
            "");
    doc_bad(kWHead, kGenBadPh, "");
    doc_bad(kWHead, kGenWidBad,
            "");
    doc_bad(kCHead, kGenBadState,
            "");
    doc_bad(kCHead, kGenBadField,
            "");
    doc_bad(kWHead, kGenSendOnly,
            "");
    assert(remove(kScratch) == 0);
    return (0);
}
