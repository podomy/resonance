// List and doctor coverage. Both are unprivileged by design.
package clitest

import (
	"strings"
	"testing"
)

func TestList(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name     string
		file     string
		wantName string
		wantSut  string
	}{
		{"agree", "../suite/examples/agree.yaml", "name=agree", "sut=concord"},
		{"update", "../suite/examples/update.yaml", "name=update", "sut=concord"},
		{"weld-roundtrip", "../suite/examples/weld_roundtrip.yaml", "name=weld-roundtrip", "sut=weld"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			code, stdout, stderr := runRSuite(t, []string{"list", tc.file})
			if code != 0 {
				t.Fatalf("list %s exit = %d, want 0 (stderr %q)", tc.file, code, stderr)
			}
			if !strings.HasPrefix(stdout, tc.file+":") {
				t.Errorf("stdout missing %q prefix, got %q", tc.file+":", stdout)
			}
			for _, want := range []string{tc.wantName, tc.wantSut, "nodes=", "phases="} {
				if !strings.Contains(stdout, want) {
					t.Errorf("stdout missing %q, got %q", want, stdout)
				}
			}
		})
	}
}

func TestDoctor(t *testing.T) {
	t.Parallel()
	code, stdout, stderr := runRSuite(t, []string{"doctor"})
	if code != 0 {
		t.Fatalf("doctor exit = %d, want 0 (stdout %q stderr %q)", code, stdout, stderr)
	}
	if !strings.Contains(stdout, "MISSING") {
		t.Errorf("doctor stdout missing MISSING lines, got %q", stdout)
	}
	if !strings.Contains(stdout, ":") {
		t.Errorf("doctor stdout missing report lines, got %q", stdout)
	}
	if stderr != "" {
		t.Errorf("doctor stderr = %q, want empty", stderr)
	}
}
