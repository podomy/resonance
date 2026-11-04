// Help text coverage for rsuite and its subcommands.
package clitest

import (
	"strings"
	"testing"
)

func TestHelp(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name string
		args []string
		want string
	}{
		{"global-long", []string{"--help"}, "usage: rsuite lint"},
		{"global-short", []string{"-h"}, "usage: rsuite lint"},
		{"lint-long", []string{"lint", "--help"}, "usage: rsuite lint"},
		{"lint-short", []string{"lint", "-h"}, "usage: rsuite lint"},
		{"run-long", []string{"run", "--help"}, "usage: rsuite run"},
		{"run-short", []string{"run", "-h"}, "usage: rsuite run"},
		{"list-long", []string{"list", "--help"}, "usage: rsuite list"},
		{"list-short", []string{"list", "-h"}, "usage: rsuite list"},
		{"doctor-long", []string{"doctor", "--help"}, "usage: rsuite doctor"},
		{"doctor-short", []string{"doctor", "-h"}, "usage: rsuite doctor"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			code, stdout, stderr := runRSuite(t, tc.args)
			if code != 0 {
				t.Fatalf("exit = %d, want 0 (stderr %q)", code, stderr)
			}
			if !strings.Contains(stdout, tc.want) {
				t.Errorf("stdout missing %q, got %q", tc.want, stdout)
			}
			if !strings.Contains(stdout, "exits: 0 ok, 1 suite failure, 2 bad usage.") {
				t.Errorf("stdout missing exits line, got %q", stdout)
			}
			if stderr != "" {
				t.Errorf("stderr = %q, want empty", stderr)
			}
		})
	}
}
