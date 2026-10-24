// Copyright (c) YugabyteDB, Inc.

package ybdb

import (
	"context"
	pb "node-agent/generated/service"
	"node-agent/util"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

// DetectTimeout is the maximum time allowed for a full detection sweep.
const DetectTimeout = 5 * time.Second

// Detector detects running YugabyteDB related processes on the node.
type Detector struct {
	sys System
}

// NewDetector returns a Detector backed by the local operating system.
func NewDetector() *Detector {
	return &Detector{sys: newOsSystem()}
}

// NewDetectorWithSystem returns a Detector backed by the given System. It is
// primarily used for testing.
func NewDetectorWithSystem(sys System) *Detector {
	return &Detector{sys: sys}
}

// allDefaultPorts maps every known default port to its role.
var allDefaultPorts = func() map[uint32]pb.YugabyteProcessRole {
	ports := make(map[uint32]pb.YugabyteProcessRole)
	for _, def := range roleDefs {
		for _, port := range def.defaultPorts {
			ports[port] = def.role
		}
	}
	return ports
}()

// roleState is the intermediate detection state for a single role.
type roleState struct {
	def         roleDef
	pids        []int
	primaryArgs []string
	listeners   []ssListener
	ports       map[uint32]bool
}

// Detect performs a read-only detection sweep and returns the status of each
// known role.
func (d *Detector) Detect(ctx context.Context) (*pb.CheckYugabyteDbStatusResponse, error) {
	ctx, cancel := context.WithTimeout(ctx, DetectTimeout)
	defer cancel()

	selfPid := d.sys.SelfPid()
	states := make(map[pb.YugabyteProcessRole]*roleState, len(roleDefs))
	pidRole := make(map[int]pb.YugabyteProcessRole)
	for _, def := range roleDefs {
		state := &roleState{def: def, ports: make(map[uint32]bool)}
		pids := d.verifiedPids(ctx, def, selfPid)
		sort.Ints(pids)
		state.pids = pids
		if len(pids) > 0 {
			// The primary pid is the lowest pid and is always pids[0]
			// after sorting, so reported Pid and config stay aligned.
			for _, pid := range pids {
				pidRole[pid] = def.role
			}
			if args, err := d.sys.ReadCmdline(pids[0]); err == nil {
				state.primaryArgs = args
			}
		}
		states[def.role] = state
	}

	listeners := d.listeners(ctx)
	otherListeners := d.attributeListeners(states, pidRole, listeners)

	response := &pb.CheckYugabyteDbStatusResponse{
		OtherListeners: otherListeners,
	}
	for _, def := range roleDefs {
		state := states[def.role]
		processStatus := d.buildProcessStatus(ctx, state)
		response.Processes = append(response.Processes, processStatus)
		if processStatus.GetRunning() {
			switch def.role {
			case pb.YugabyteProcessRole_YB_ROLE_MASTER:
				response.MasterPresent = true
			case pb.YugabyteProcessRole_YB_ROLE_TSERVER:
				response.TserverPresent = true
			case pb.YugabyteProcessRole_YB_ROLE_CONTROLLER:
				response.ControllerPresent = true
			case pb.YugabyteProcessRole_YB_ROLE_POSTGRES:
				response.PostgresPresent = true
			case pb.YugabyteProcessRole_YB_ROLE_NODE_EXPORTER:
				response.NodeExporterPresent = true
			}
		}
	}
	return response, nil
}

// verifiedPids returns the pids for the role after confirming each candidate by
// classifying its command line. The current process is always excluded.
func (d *Detector) verifiedPids(
	ctx context.Context,
	def roleDef,
	selfPid int,
) []int {
	candidates, err := d.sys.Pgrep(ctx, def.pgrepPattern)
	if err != nil {
		util.FileLogger().Warnf(ctx, "Failed to pgrep for %s: %s", def.name, err.Error())
		return nil
	}
	pids := make([]int, 0, len(candidates))
	for _, pid := range candidates {
		if pid == selfPid {
			continue
		}
		args, err := d.sys.ReadCmdline(pid)
		if err != nil {
			continue
		}
		if classifyRole(args) == def.role {
			pids = append(pids, pid)
		}
	}
	return pids
}

// listeners returns the parsed listening sockets. A failure to run the sweep is
// logged and treated as an empty result so that process based detection can
// still succeed.
func (d *Detector) listeners(ctx context.Context) []ssListener {
	out, err := d.sys.SsListeners(ctx)
	if err != nil {
		util.FileLogger().Warnf(ctx, "Failed to list listening sockets: %s", err.Error())
		return nil
	}
	return parseSsListeners(out)
}

// attributeListeners assigns listening sockets to roles. A listener is only
// credited to a role when classifyListener agrees, so an unrelated process on
// a default YB port (9000, 5433, …) cannot mark that role present. The
// exception is a pid-less default-port listener (ss -p unavailable) when the
// role was already confirmed via pgrep.
func (d *Detector) attributeListeners(
	states map[pb.YugabyteProcessRole]*roleState,
	pidRole map[int]pb.YugabyteProcessRole,
	listeners []ssListener,
) []*pb.ProcessListener {
	otherListeners := make([]*pb.ProcessListener, 0)
	for _, listener := range listeners {
		classified := d.classifyListener(listener, pidRole)
		if classified != pb.YugabyteProcessRole_YB_ROLE_UNKNOWN {
			appendListener(states[classified], listener, classified, &otherListeners)
			continue
		}
		if listener.pid != 0 {
			continue
		}
		role, isDefault := allDefaultPorts[listener.port]
		if !isDefault || len(states[role].pids) == 0 {
			continue
		}
		appendListener(states[role], listener, role, &otherListeners)
	}
	return otherListeners
}

// appendListener records a listener on its classified role. Default ports for
// that role are kept on the process status; any other port is also reported in
// otherListeners.
func appendListener(
	state *roleState,
	listener ssListener,
	role pb.YugabyteProcessRole,
	otherListeners *[]*pb.ProcessListener,
) {
	if owner, isDefault := allDefaultPorts[listener.port]; isDefault && owner == role {
		state.ports[listener.port] = true
		state.listeners = append(state.listeners, listener)
		return
	}
	*otherListeners = append(*otherListeners, toProtoListener(listener, role))
}

// classifyListener determines the role of a listening socket using the pid to
// role map, then the process command line, then the role of a verified parent
// process and finally the process name.
func (d *Detector) classifyListener(
	listener ssListener,
	pidRole map[int]pb.YugabyteProcessRole,
) pb.YugabyteProcessRole {
	if listener.pid == 0 {
		return pb.YugabyteProcessRole_YB_ROLE_UNKNOWN
	}
	if role, ok := pidRole[listener.pid]; ok {
		return role
	}
	args, cmdlineErr := d.sys.ReadCmdline(listener.pid)
	if cmdlineErr == nil {
		if role := classifyRole(args); role != pb.YugabyteProcessRole_YB_ROLE_UNKNOWN {
			return role
		}
	}
	// Forked helpers such as the YSQL webserver (a postgres background worker)
	// rewrite their cmdline, so they are attributed through a verified parent.
	if ppid, err := d.sys.ReadPpid(listener.pid); err == nil {
		if role, ok := pidRole[ppid]; ok {
			return role
		}
	}
	if cmdlineErr == nil {
		// Trust the executable path over the kernel comm, including a
		// definitive UNKNOWN (e.g. system postgres vs yb-postmaster).
		return pb.YugabyteProcessRole_YB_ROLE_UNKNOWN
	}
	return classifyProcessName(listener.processName)
}

// buildProcessStatus assembles the status for a single role, including the
// configuration when applicable.
func (d *Detector) buildProcessStatus(
	ctx context.Context,
	state *roleState,
) *pb.ProcessStatus {
	// Ports are only populated from classified (or pgrep-backed pid-less)
	// listeners, so a foreign process on a default YB port cannot flip this.
	running := len(state.pids) > 0 || len(state.ports) > 0
	status := &pb.ProcessStatus{
		Role:    state.def.role,
		Running: running,
	}
	if len(state.pids) > 0 {
		status.Pid = int32(state.pids[0])
	}
	ports := make([]uint32, 0, len(state.ports))
	for port := range state.ports {
		ports = append(ports, port)
	}
	sort.Slice(ports, func(i, j int) bool { return ports[i] < ports[j] })
	status.ListenPorts = ports
	for _, listener := range state.listeners {
		status.Listeners = append(status.Listeners, toProtoListener(listener, state.def.role))
	}
	if state.def.hasConfig && running && len(state.primaryArgs) > 0 {
		status.Config = d.buildConfigForProcess(ctx, state)
	}
	return status
}

// buildConfigForProcess extracts the flagfile path and arguments for a running
// process from its command line and flagfile.
func (d *Detector) buildConfigForProcess(
	ctx context.Context,
	state *roleState,
) *pb.ProcessConfig {
	flagfilePath, cmdArgs := parseCmdline(state.primaryArgs)
	if flagfilePath == "" {
		flagfilePath = deriveFlagfilePath(state.primaryArgs[0])
	}
	var exists, readable bool
	var content []byte
	if flagfilePath != "" {
		exists, readable = d.sys.StatFile(flagfilePath)
		if readable {
			data, err := d.sys.ReadFileLimit(flagfilePath, maxFlagfileBytes)
			if err != nil {
				util.FileLogger().
					Warnf(ctx, "Failed to read flagfile %s: %s", flagfilePath, err.Error())
				readable = false
			} else {
				content = data
			}
		}
	}
	return buildConfig(flagfilePath, exists, readable, cmdArgs, content)
}

// deriveFlagfilePath returns the conventional flagfile path for a YugabyteDB
// binary, e.g. ".../master/bin/yb-master" resolves to
// ".../master/conf/server.conf".
func deriveFlagfilePath(binaryPath string) string {
	if binaryPath == "" {
		return ""
	}
	binDir := filepath.Dir(binaryPath)
	serverHome := filepath.Dir(binDir)
	if serverHome == "" || serverHome == "." || serverHome == "/" {
		return ""
	}
	return filepath.Join(serverHome, "conf", "server.conf")
}

// classifyProcessName classifies a listening socket by its process name, which
// may be truncated to the kernel comm length.
func classifyProcessName(name string) pb.YugabyteProcessRole {
	if name == "" {
		return pb.YugabyteProcessRole_YB_ROLE_UNKNOWN
	}
	switch {
	case strings.HasPrefix(name, "yb-master"):
		return pb.YugabyteProcessRole_YB_ROLE_MASTER
	case strings.HasPrefix(name, "yb-tserver"):
		return pb.YugabyteProcessRole_YB_ROLE_TSERVER
	case strings.HasPrefix(name, "yb-controller"):
		return pb.YugabyteProcessRole_YB_ROLE_CONTROLLER
	case strings.Contains(name, "postgres") || strings.Contains(name, "postmaster"):
		return pb.YugabyteProcessRole_YB_ROLE_POSTGRES
	case strings.Contains(name, "node_exporter"):
		return pb.YugabyteProcessRole_YB_ROLE_NODE_EXPORTER
	}
	return pb.YugabyteProcessRole_YB_ROLE_UNKNOWN
}

// toProtoListener converts an internal listener to its proto form.
func toProtoListener(listener ssListener, role pb.YugabyteProcessRole) *pb.ProcessListener {
	return &pb.ProcessListener{
		Port:        listener.port,
		Protocol:    listener.protocol,
		Pid:         int32(listener.pid),
		Role:        role,
		ProcessName: listener.processName,
	}
}
