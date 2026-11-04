// Lint coverage: valid fixtures pass, crafted invalid files fail
// with file:line diagnostics on stderr.
package clitest

import (
	"strings"
	"testing"
)

func TestLintValid(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name string
		file string
	}{
		{"agree", "../suite/examples/agree.yaml"},
		{"bench-concord", "../suite/examples/bench_concord.yaml"},
		{"bench-weld", "../suite/examples/bench_weld.yaml"},
		{"bulk", "../suite/examples/bulk.yaml"},
		{"concord-two", "../suite/examples/concord_two.yaml"},
		{"faults", "../suite/examples/faults.yaml"},
		{"skew", "../suite/examples/skew.yaml"},
		{"update", "../suite/examples/update.yaml"},
		{"weld-roundtrip", "../suite/examples/weld_roundtrip.yaml"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			code, stdout, stderr := runRSuite(t, []string{"lint", tc.file})
			if code != 0 {
				t.Fatalf("lint %s exit = %d, want 0 (stderr %q)", tc.file, code, stderr)
			}
			if !strings.Contains(stdout, ": OK") {
				t.Errorf("stdout missing ': OK', got %q", stdout)
			}
			if stderr != "" {
				t.Errorf("stderr = %q, want empty", stderr)
			}
		})
	}
}

func TestLintInvalid(t *testing.T) {
	t.Parallel()
	const cHead = "name: t\nversion: 1\nsut: concord\nnodes: 2\ntimeout_s: 10\nphases:\n"
	const mesh = "  - name: mesh\n    until:\n      peers: 2\n      on: all\n    timeout_s: 10\n"
	cases := []struct {
		name string
		body string
		want string
	}{
		{
			"unknown-key",
			cHead + mesh + "bogus: 1\n",
			"unknown top-level key",
		},
		{
			"dup-phase",
			cHead + mesh + "  - name: mesh\n    until:\n      mesh_count: 1\n    timeout_s: 10\n",
			"duplicate phase name",
		},
		{
			"bad-version",
			"name: t\nversion: 2\nsut: concord\nnodes: 2\ntimeout_s: 10\nphases:\n" + mesh,
			"version must equal 1",
		},
		{
			"index-range",
			cHead + "  - name: d\n    do:\n      - journalcorrupt:\n          node: 9\n    until:\n      mesh_count: 1\n    timeout_s: 10\n",
			"node index out of range",
		},
		{
			"timeout-range",
			"name: t\nversion: 1\nsut: concord\nnodes: 2\ntimeout_s: 0\nphases:\n" + mesh,
			"timeout_s must be 1..3600",
		},
		{
			"domain",
			"name: t\nversion: 1\nsut: concord\nnodes: 2\ndomain: 64\ntimeout_s: 10\nphases:\n" + mesh,
			"domain is weld-only",
		},
		{
			"second-submit",
			cHead + "  - name: a\n    do:\n      - submit:\n          node: 0\n          image: probe\n    until:\n      mesh_count: 1\n    timeout_s: 10\n" + "  - name: b\n    do:\n      - submit:\n          node: 0\n          image: probe\n    until:\n      mesh_count: 1\n    timeout_s: 10\n",
			"second submit needs as",
		},
		{
			"stop-before-submit",
			cHead + "  - name: b\n    do:\n      - stop:\n          workload: submitted\n    until:\n      mesh_count: 1\n    timeout_s: 10\n",
			"stop with no submitted workload",
		},
		{
			"phase-timeout-range",
			cHead + "  - name: mesh\n    until:\n      peers: 2\n      on: all\n    timeout_s: 3601\n",
			"timeout_s must be 1..3600",
		},
		{
			"nodes-range",
			"name: t\nversion: 1\nsut: concord\nnodes: 101\ntimeout_s: 10\nphases:\n" + mesh,
			"nodes must be 1..100",
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			path := writeTempYAML(t, tc.body)
			code, stdout, stderr := runRSuite(t, []string{"lint", path})
			if code != 1 {
				t.Fatalf("lint invalid %s exit = %d, want 1 (stdout %q stderr %q)", tc.name, code, stdout, stderr)
			}
			if !strings.Contains(stderr, ".yaml:") {
				t.Errorf("stderr missing file:line (.yaml:), got %q", stderr)
			}
			if !strings.Contains(stderr, tc.want) {
				t.Errorf("stderr missing %q, got %q", tc.want, stderr)
			}
		})
	}
}
