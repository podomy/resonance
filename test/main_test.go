// Package clitest exercises bin/rsuite as a black box.
package clitest

import (
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

// rsuiteBin is the CLI under test, relative to this package dir.
var rsuiteBin = filepath.Join("..", "bin", "rsuite")

// TestMain builds the binary once and fails fast on build error.
func TestMain(m *testing.M) {
	cmd := exec.Command("make", "-C", "..", "bin/rsuite")
	var out bytes.Buffer
	cmd.Stdout = &out
	cmd.Stderr = &out
	if err := cmd.Run(); err != nil {
		os.Stderr.WriteString("build bin/rsuite failed: " + err.Error() + "\n" + out.String())
		os.Exit(1)
	}
	os.Exit(m.Run())
}

// runRSuite runs the binary with an argv array (no shell) and
// returns the exit code plus split stdout/stderr.
func runRSuite(t *testing.T, args []string) (int, string, string) {
	t.Helper()
	cmd := exec.Command(rsuiteBin, args...)
	var stdout, stderr bytes.Buffer
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	err := cmd.Run()
	if err == nil {
		return 0, stdout.String(), stderr.String()
	}
	if exitErr, ok := err.(*exec.ExitError); ok {
		return exitErr.ExitCode(), stdout.String(), stderr.String()
	}
	t.Fatalf("exec %v failed: %v", args, err)
	return -1, "", ""
}

// writeTempYAML writes content to a .yaml file under t.TempDir.
func writeTempYAML(t *testing.T, content string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "case.yaml")
	if err := os.WriteFile(path, []byte(content), 0600); err != nil {
		t.Fatalf("write temp yaml: %v", err)
	}
	return path
}
