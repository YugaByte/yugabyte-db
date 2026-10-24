// Copyright (c) YugabyteDB, Inc.

package ybdb

import (
	"context"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
)

// hardenedPath lists the standard directories where ss and pgrep are installed
// across RHEL and Debian based distributions. It is used to resolve the tools
// and is placed on the command environment so that detection works even when
// the node-agent process runs with a minimal PATH (for example under systemd).
const hardenedPath = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

// resolveTool returns an absolute path to the given tool. It first honors the
// process PATH and falls back to the hardened directories. Go resolves the
// binary at command construction time using the process PATH, so an explicit
// lookup is required for the hardened directories to take effect. The bare name
// is returned when the tool cannot be found so that exec surfaces a clear error.
func resolveTool(name string) string {
	if path, err := exec.LookPath(name); err == nil {
		return path
	}
	for _, dir := range filepath.SplitList(hardenedPath) {
		candidate := filepath.Join(dir, name)
		if info, err := os.Stat(candidate); err == nil && !info.IsDir() &&
			info.Mode()&0111 != 0 {
			return candidate
		}
	}
	return name
}

// commandEnv returns the process environment with the hardened directories
// ensured on PATH without changing the precedence of the existing PATH.
func commandEnv() []string {
	merged := hardenedPath
	if existing := os.Getenv("PATH"); existing != "" {
		merged = existing + string(os.PathListSeparator) + hardenedPath
	}
	env := os.Environ()
	result := make([]string, 0, len(env)+1)
	replaced := false
	for _, kv := range env {
		if strings.HasPrefix(kv, "PATH=") {
			result = append(result, "PATH="+merged)
			replaced = true
			continue
		}
		result = append(result, kv)
	}
	if !replaced {
		result = append(result, "PATH="+merged)
	}
	return result
}

// System abstracts the operating system interactions needed for detection so
// that the detector can be unit tested with fixtures.
type System interface {
	// Pgrep returns the pids whose full command line matches the pattern. No
	// match is not an error and returns an empty slice.
	Pgrep(ctx context.Context, pattern string) ([]int, error)
	// ReadCmdline returns the NUL separated arguments of a process.
	ReadCmdline(pid int) ([]string, error)
	// ReadPpid returns the parent pid of a process.
	ReadPpid(pid int) (int, error)
	// SsListeners returns the raw output of the listening socket sweep.
	SsListeners(ctx context.Context) (string, error)
	// StatFile reports whether the path exists and is readable.
	StatFile(path string) (exists bool, readable bool)
	// ReadFileLimit reads up to max bytes from the path.
	ReadFileLimit(path string, max int64) ([]byte, error)
	// SelfPid returns the pid of the current process.
	SelfPid() int
}

// osSystem is the production System backed by the local operating system.
type osSystem struct{}

func newOsSystem() System {
	return &osSystem{}
}

func (s *osSystem) Pgrep(ctx context.Context, pattern string) ([]int, error) {
	cmd := exec.CommandContext(ctx, resolveTool("pgrep"), "-f", pattern)
	cmd.Env = commandEnv()
	out, err := cmd.Output()
	if err != nil {
		// pgrep exits with 1 when there is no match, which is not an error here.
		if exitErr, ok := err.(*exec.ExitError); ok && exitErr.ExitCode() == 1 {
			return nil, nil
		}
		return nil, err
	}
	pids := make([]int, 0)
	for _, line := range strings.Split(string(out), "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		if pid, err := strconv.Atoi(line); err == nil {
			pids = append(pids, pid)
		}
	}
	return pids, nil
}

func (s *osSystem) ReadCmdline(pid int) ([]string, error) {
	data, err := os.ReadFile("/proc/" + strconv.Itoa(pid) + "/cmdline")
	if err != nil {
		return nil, err
	}
	if len(data) == 0 {
		return nil, nil
	}
	trimmed := strings.TrimSuffix(string(data), "\x00")
	return strings.Split(trimmed, "\x00"), nil
}

func (s *osSystem) ReadPpid(pid int) (int, error) {
	path := "/proc/" + strconv.Itoa(pid) + "/stat"
	data, err := os.ReadFile(path)
	if err != nil {
		return 0, err
	}
	ppid, ok := parsePpid(string(data))
	if !ok {
		return 0, fmt.Errorf("unable to parse parent pid from %s", path)
	}
	return ppid, nil
}

func (s *osSystem) SsListeners(ctx context.Context) (string, error) {
	cmd := exec.CommandContext(ctx, resolveTool("ss"), "-H", "-O", "-lntp")
	cmd.Env = commandEnv()
	out, err := cmd.Output()
	if err != nil {
		return "", err
	}
	return string(out), nil
}

func (s *osSystem) StatFile(path string) (bool, bool) {
	info, err := os.Stat(path)
	if err != nil || info.IsDir() {
		return false, false
	}
	file, err := os.Open(path)
	if err != nil {
		return true, false
	}
	file.Close()
	return true, true
}

func (s *osSystem) ReadFileLimit(path string, max int64) ([]byte, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	return io.ReadAll(io.LimitReader(file, max))
}

func (s *osSystem) SelfPid() int {
	return os.Getpid()
}
