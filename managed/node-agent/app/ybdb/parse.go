// Copyright (c) YugabyteDB, Inc.

package ybdb

import (
	"regexp"
	"strconv"
	"strings"
)

// ssListener is a single listening socket parsed from ss output.
type ssListener struct {
	port        uint32
	protocol    string
	pid         int
	processName string
}

var (
	// ssProcessPidRegex extracts the pid from an ss users:(("name",pid=123,fd=4)) column.
	ssProcessPidRegex = regexp.MustCompile(`pid=(\d+)`)
	// ssProcessNameRegex extracts the process name from the same column.
	ssProcessNameRegex = regexp.MustCompile(`\("([^"]+)"`)
)

// parsePpid extracts the parent pid from the contents of /proc/<pid>/stat. The
// comm field may contain spaces and parentheses, so parsing starts after the
// last closing parenthesis: "<pid> (<comm>) <state> <ppid> ...".
func parsePpid(stat string) (int, bool) {
	end := strings.LastIndexByte(stat, ')')
	if end < 0 {
		return 0, false
	}
	fields := strings.Fields(stat[end+1:])
	if len(fields) < 2 {
		return 0, false
	}
	ppid, err := strconv.Atoi(fields[1])
	if err != nil {
		return 0, false
	}
	return ppid, true
}

// parseSsListeners parses the output of "ss -H -O -lntp". Lines that cannot be
// parsed are skipped. The process column is optional as it is only present when
// the caller has sufficient privilege.
func parseSsListeners(output string) []ssListener {
	listeners := make([]ssListener, 0)
	for _, line := range strings.Split(output, "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		fields := strings.Fields(line)
		// State Recv-Q Send-Q Local-Address:Port Peer-Address:Port [Process]
		if len(fields) < 4 {
			continue
		}
		localAddr := fields[3]
		port, ok := portFromAddress(localAddr)
		if !ok {
			continue
		}
		listener := ssListener{
			port:     port,
			protocol: protocolFromAddress(localAddr),
		}
		// The process column, when present, is the remaining text after the peer.
		if len(fields) >= 6 {
			processCol := strings.Join(fields[5:], " ")
			if match := ssProcessPidRegex.FindStringSubmatch(processCol); match != nil {
				if pid, err := strconv.Atoi(match[1]); err == nil {
					listener.pid = pid
				}
			}
			if match := ssProcessNameRegex.FindStringSubmatch(processCol); match != nil {
				listener.processName = match[1]
			}
		}
		listeners = append(listeners, listener)
	}
	return listeners
}

// portFromAddress returns the port from an ss local address such as
// "0.0.0.0:7000", "[::]:7100", "*:9300" or "127.0.0.1:5433".
func portFromAddress(addr string) (uint32, bool) {
	idx := strings.LastIndex(addr, ":")
	if idx < 0 || idx == len(addr)-1 {
		return 0, false
	}
	port, err := strconv.ParseUint(addr[idx+1:], 10, 32)
	if err != nil {
		return 0, false
	}
	return uint32(port), true
}

// protocolFromAddress returns "tcp6" for IPv6 style addresses and "tcp" otherwise.
func protocolFromAddress(addr string) string {
	if strings.Contains(addr, "[") || strings.Contains(addr, "::") {
		return "tcp6"
	}
	return "tcp"
}

// parseCmdline parses the arguments of a process command line (excluding the
// binary path in args[0]). It returns the resolved --flagfile path and the list
// of remaining arguments as key-value pairs. Both "--key=value" and
// "--key value" forms are supported.
func parseCmdline(args []string) (flagfilePath string, cmdArgs []argKV) {
	cmdArgs = make([]argKV, 0)
	// Skip args[0] which is the binary path.
	for i := 1; i < len(args); i++ {
		token := args[i]
		if !strings.HasPrefix(token, "-") {
			continue
		}
		key := strings.TrimLeft(token, "-")
		if key == "" {
			continue
		}
		var value string
		if eq := strings.Index(key, "="); eq >= 0 {
			value = key[eq+1:]
			key = key[:eq]
		} else if i+1 < len(args) && !strings.HasPrefix(args[i+1], "-") {
			// Space separated value, e.g. "--flagfile /path".
			value = args[i+1]
			i++
		}
		if key == "flagfile" {
			flagfilePath = value
		}
		cmdArgs = append(cmdArgs, argKV{key: key, value: value})
	}
	return flagfilePath, cmdArgs
}

// parseFlagfile parses a gflags file into key-value pairs. Blank lines and
// comment lines (starting with '#' or '//') are ignored. Each flag may be written as
// "--key=value", "key=value" or "--key" (boolean).
func parseFlagfile(content []byte) []argKV {
	args := make([]argKV, 0)
	for _, line := range strings.Split(string(content), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") || strings.HasPrefix(line, "//") {
			continue
		}
		key := strings.TrimLeft(line, "-")
		if key == "" {
			continue
		}
		var value string
		if eq := strings.Index(key, "="); eq >= 0 {
			value = strings.TrimSpace(key[eq+1:])
			key = strings.TrimSpace(key[:eq])
		}
		if key == "" {
			continue
		}
		args = append(args, argKV{key: key, value: value})
	}
	return args
}
