// Copyright (c) YugabyteDB, Inc.

package ybdb

import (
	"os"
	"strings"
	"testing"
)

func TestResolveToolFallsBackToHardenedPath(t *testing.T) {
	// A tool that is very unlikely to be on PATH resolves to the bare name.
	if got := resolveTool("definitely-not-a-real-tool-xyz"); got != "definitely-not-a-real-tool-xyz" {
		t.Errorf("resolveTool() = %q, want bare name", got)
	}
	// A tool that exists in a hardened directory resolves to an absolute path.
	// "sh" is present in /bin on both RHEL and Debian based systems.
	if got := resolveTool("sh"); !strings.HasPrefix(got, "/") {
		t.Errorf("resolveTool(\"sh\") = %q, want an absolute path", got)
	}
}

func TestCommandEnvEnsuresHardenedPath(t *testing.T) {
	env := commandEnv()
	var pathValue string
	pathCount := 0
	for _, kv := range env {
		if strings.HasPrefix(kv, "PATH=") {
			pathValue = strings.TrimPrefix(kv, "PATH=")
			pathCount++
		}
	}
	if pathCount != 1 {
		t.Fatalf("commandEnv() has %d PATH entries, want exactly 1", pathCount)
	}
	for _, dir := range strings.Split(hardenedPath, string(os.PathListSeparator)) {
		if !strings.Contains(pathValue, dir) {
			t.Errorf("commandEnv() PATH %q missing hardened dir %q", pathValue, dir)
		}
	}
}
