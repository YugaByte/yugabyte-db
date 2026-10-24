// Copyright (c) YugabyteDB, Inc.

package ybdb

import (
	"context"
	pb "node-agent/generated/service"
	"testing"
)

// fakeSystem is an in-memory System used to drive the detector in tests.
type fakeSystem struct {
	pgrep    map[string][]int
	cmdlines map[int][]string
	ppids    map[int]int
	ssOutput string
	files    map[string][]byte
	selfPid  int
}

func (f *fakeSystem) Pgrep(ctx context.Context, pattern string) ([]int, error) {
	return f.pgrep[pattern], nil
}

func (f *fakeSystem) ReadCmdline(pid int) ([]string, error) {
	if args, ok := f.cmdlines[pid]; ok {
		return args, nil
	}
	return nil, context.Canceled
}

func (f *fakeSystem) ReadPpid(pid int) (int, error) {
	if ppid, ok := f.ppids[pid]; ok {
		return ppid, nil
	}
	return 0, context.Canceled
}

func (f *fakeSystem) SsListeners(ctx context.Context) (string, error) {
	return f.ssOutput, nil
}

func (f *fakeSystem) StatFile(path string) (bool, bool) {
	_, ok := f.files[path]
	return ok, ok
}

func (f *fakeSystem) ReadFileLimit(path string, max int64) ([]byte, error) {
	if data, ok := f.files[path]; ok {
		return data, nil
	}
	return nil, context.Canceled
}

func (f *fakeSystem) SelfPid() int {
	return f.selfPid
}

func fullDeploymentSystem() *fakeSystem {
	return &fakeSystem{
		selfPid: 999,
		pgrep: map[string][]int{
			"bin/yb-master":            {1001},
			"bin/yb-tserver":           {1002},
			"bin/yb-controller-server": {1003},
			"postgres/bin/postgres":    {1004},
			"node_exporter":            {1005},
		},
		cmdlines: map[int][]string{
			1001: {
				"/home/yugabyte/master/bin/yb-master",
				"--flagfile",
				"/home/yugabyte/master/conf/server.conf",
			},
			1002: {
				"/home/yugabyte/tserver/bin/yb-tserver",
				"--flagfile",
				"/home/yugabyte/tserver/conf/server.conf",
			},
			1003: {
				"/home/yugabyte/controller/bin/yb-controller-server",
				"--flagfile",
				"/home/yugabyte/controller/conf/server.conf",
			},
			1004: {"/home/yugabyte/tserver/postgres/bin/postgres", "-D", "/home/yugabyte/pgdata"},
			1005: {"/home/yugabyte/node_exporter/bin/node_exporter"},
		},
		ssOutput: `LISTEN 0 4096 0.0.0.0:7000 0.0.0.0:* users:(("yb-master",pid=1001,fd=1))
LISTEN 0 4096 0.0.0.0:7100 0.0.0.0:* users:(("yb-master",pid=1001,fd=2))
LISTEN 0 4096 0.0.0.0:9000 0.0.0.0:* users:(("yb-tserver",pid=1002,fd=1))
LISTEN 0 4096 0.0.0.0:9100 0.0.0.0:* users:(("yb-tserver",pid=1002,fd=2))
LISTEN 0 4096 0.0.0.0:14000 0.0.0.0:* users:(("yb-controller-s",pid=1003,fd=1))
LISTEN 0 4096 0.0.0.0:18018 0.0.0.0:* users:(("yb-controller-s",pid=1003,fd=2))
LISTEN 0 128 127.0.0.1:5433 0.0.0.0:* users:(("postgres",pid=1004,fd=1))
LISTEN 0 128 0.0.0.0:9300 0.0.0.0:* users:(("node_exporter",pid=1005,fd=1))
LISTEN 0 128 0.0.0.0:9042 0.0.0.0:* users:(("yb-tserver",pid=1002,fd=9))
`,
		files: map[string][]byte{
			"/home/yugabyte/master/conf/server.conf": []byte(
				"--rpc_bind_addresses=10.0.0.1:7100\n--webserver_port=7000\n",
			),
			"/home/yugabyte/tserver/conf/server.conf": []byte(
				"--rpc_bind_addresses=10.0.0.1:9100\n--webserver_port=9000\n",
			),
			"/home/yugabyte/controller/conf/server.conf": []byte("--server_address=10.0.0.1\n"),
		},
	}
}

func statusForRole(
	res *pb.CheckYugabyteDbStatusResponse,
	role pb.YugabyteProcessRole,
) *pb.ProcessStatus {
	for _, status := range res.GetProcesses() {
		if status.GetRole() == role {
			return status
		}
	}
	return nil
}

func TestDetectFullDeployment(t *testing.T) {
	detector := NewDetectorWithSystem(fullDeploymentSystem())
	res, err := detector.Detect(context.Background())
	if err != nil {
		t.Fatalf("Detect() error = %v", err)
	}
	if !res.GetMasterPresent() || !res.GetTserverPresent() || !res.GetControllerPresent() ||
		!res.GetPostgresPresent() || !res.GetNodeExporterPresent() {
		t.Fatalf("expected all roles present, got %+v", res)
	}

	master := statusForRole(res, pb.YugabyteProcessRole_YB_ROLE_MASTER)
	if master == nil || !master.GetRunning() || master.GetPid() != 1001 {
		t.Fatalf("unexpected master status %+v", master)
	}
	if !equalPorts(master.GetListenPorts(), []uint32{7000, 7100}) {
		t.Errorf("master listen ports = %v, want [7000 7100]", master.GetListenPorts())
	}
	if master.GetConfig() == nil ||
		master.GetConfig().GetFlagfilePath() != "/home/yugabyte/master/conf/server.conf" {
		t.Errorf("unexpected master config %+v", master.GetConfig())
	}
	if !hasArg(master.GetConfig().GetFlagfileArgs(), "rpc_bind_addresses", "10.0.0.1:7100") {
		t.Errorf("master flagfile args missing rpc_bind_addresses: %+v",
			master.GetConfig().GetFlagfileArgs())
	}

	postgres := statusForRole(res, pb.YugabyteProcessRole_YB_ROLE_POSTGRES)
	if postgres == nil || !postgres.GetRunning() || postgres.GetPid() != 1004 {
		t.Fatalf("unexpected postgres status %+v", postgres)
	}
	if postgres.GetConfig() != nil {
		t.Errorf("postgres should not have config, got %+v", postgres.GetConfig())
	}
	if !equalPorts(postgres.GetListenPorts(), []uint32{5433}) {
		t.Errorf("postgres listen ports = %v, want [5433]", postgres.GetListenPorts())
	}

	// The non-default tserver port must appear in otherListeners.
	if !hasListenerPort(res.GetOtherListeners(), 9042, pb.YugabyteProcessRole_YB_ROLE_TSERVER) {
		t.Errorf("expected other listener on 9042 for tserver, got %+v", res.GetOtherListeners())
	}
}

func TestDetectNothingRunning(t *testing.T) {
	detector := NewDetectorWithSystem(&fakeSystem{selfPid: 999, pgrep: map[string][]int{}})
	res, err := detector.Detect(context.Background())
	if err != nil {
		t.Fatalf("Detect() error = %v", err)
	}
	if res.GetMasterPresent() || res.GetTserverPresent() || res.GetControllerPresent() ||
		res.GetPostgresPresent() || res.GetNodeExporterPresent() {
		t.Fatalf("expected no roles present, got %+v", res)
	}
	for _, status := range res.GetProcesses() {
		if status.GetRunning() {
			t.Errorf("role %s should not be running", status.GetRole())
		}
		if status.GetConfig() != nil {
			t.Errorf("role %s should not have config", status.GetRole())
		}
	}
}

func TestDetectExcludesSelfPid(t *testing.T) {
	sys := &fakeSystem{
		selfPid: 1001,
		pgrep:   map[string][]int{"bin/yb-master": {1001}},
		cmdlines: map[int][]string{
			1001: {"/home/yugabyte/master/bin/yb-master", "--flagfile", "/conf/server.conf"},
		},
	}
	detector := NewDetectorWithSystem(sys)
	res, err := detector.Detect(context.Background())
	if err != nil {
		t.Fatalf("Detect() error = %v", err)
	}
	if res.GetMasterPresent() {
		t.Errorf("self pid must be excluded from detection")
	}
}

func TestDetectIgnoresUnrelatedProcessOnDefaultPort(t *testing.T) {
	sys := &fakeSystem{
		selfPid: 999,
		pgrep:   map[string][]int{},
		cmdlines: map[int][]string{
			2001: {"/usr/sbin/nginx"},
			2002: {"/usr/lib/postgresql/16/bin/postgres", "-D", "/var/lib/postgresql/data"},
		},
		ssOutput: `LISTEN 0 4096 0.0.0.0:9000 0.0.0.0:* users:(("nginx",pid=2001,fd=1))
LISTEN 0 128 127.0.0.1:5433 0.0.0.0:* users:(("postgres",pid=2002,fd=1))
`,
	}
	detector := NewDetectorWithSystem(sys)
	res, err := detector.Detect(context.Background())
	if err != nil {
		t.Fatalf("Detect() error = %v", err)
	}
	if res.GetTserverPresent() {
		t.Errorf("unrelated process on 9000 must not mark tserver present")
	}
	if res.GetPostgresPresent() {
		t.Errorf("system postgres on 5433 must not mark YB postgres present")
	}
	tserver := statusForRole(res, pb.YugabyteProcessRole_YB_ROLE_TSERVER)
	if tserver != nil && len(tserver.GetListeners()) > 0 {
		t.Errorf("tserver must not inherit foreign listeners, got %+v", tserver.GetListeners())
	}
}

func TestDetectPidMatchesPrimaryConfig(t *testing.T) {
	sys := &fakeSystem{
		selfPid: 999,
		pgrep:   map[string][]int{"bin/yb-master": {2002, 1001}},
		cmdlines: map[int][]string{
			2002: {
				"/home/yugabyte/master/bin/yb-master",
				"--flagfile",
				"/home/yugabyte/master/conf/newer.conf",
			},
			1001: {
				"/home/yugabyte/master/bin/yb-master",
				"--flagfile",
				"/home/yugabyte/master/conf/server.conf",
			},
		},
		files: map[string][]byte{
			"/home/yugabyte/master/conf/server.conf": []byte("--webserver_port=7000\n"),
			"/home/yugabyte/master/conf/newer.conf":  []byte("--webserver_port=7001\n"),
		},
	}
	detector := NewDetectorWithSystem(sys)
	res, err := detector.Detect(context.Background())
	if err != nil {
		t.Fatalf("Detect() error = %v", err)
	}
	master := statusForRole(res, pb.YugabyteProcessRole_YB_ROLE_MASTER)
	if master == nil || !master.GetRunning() {
		t.Fatalf("expected running master, got %+v", master)
	}
	if master.GetPid() != 1001 {
		t.Errorf("Pid = %d, want primary pid 1001", master.GetPid())
	}
	if master.GetConfig() == nil ||
		master.GetConfig().GetFlagfilePath() != "/home/yugabyte/master/conf/server.conf" {
		t.Errorf("config must come from primary pid, got %+v", master.GetConfig())
	}
}

func TestDetectPidlessDefaultPortRequiresVerifiedProcess(t *testing.T) {
	// ss without -p (pid=0) on a default port must not claim the role unless
	// pgrep already confirmed a matching process.
	unrelated := &fakeSystem{
		selfPid:  999,
		pgrep:    map[string][]int{},
		ssOutput: "LISTEN 0 4096 0.0.0.0:9000 0.0.0.0:*\n",
	}
	res, err := NewDetectorWithSystem(unrelated).Detect(context.Background())
	if err != nil {
		t.Fatalf("Detect() error = %v", err)
	}
	if res.GetTserverPresent() {
		t.Errorf("pid-less listener on 9000 must not mark tserver present")
	}

	confirmed := &fakeSystem{
		selfPid: 999,
		pgrep:   map[string][]int{"bin/yb-tserver": {1002}},
		cmdlines: map[int][]string{
			1002: {
				"/home/yugabyte/tserver/bin/yb-tserver",
				"--flagfile",
				"/home/yugabyte/tserver/conf/server.conf",
			},
		},
		ssOutput: "LISTEN 0 4096 0.0.0.0:9000 0.0.0.0:*\n",
		files: map[string][]byte{
			"/home/yugabyte/tserver/conf/server.conf": []byte("--webserver_port=9000\n"),
		},
	}
	res, err = NewDetectorWithSystem(confirmed).Detect(context.Background())
	if err != nil {
		t.Fatalf("Detect() error = %v", err)
	}
	if !res.GetTserverPresent() {
		t.Fatalf("verified tserver must still be present without ss -p")
	}
	tserver := statusForRole(res, pb.YugabyteProcessRole_YB_ROLE_TSERVER)
	if tserver == nil || !equalPorts(tserver.GetListenPorts(), []uint32{9000}) {
		t.Errorf("tserver listen ports = %v, want [9000]", tserver.GetListenPorts())
	}
}

func TestDetectAttributesForkedChildToVerifiedParent(t *testing.T) {
	sys := &fakeSystem{
		selfPid: 999,
		pgrep:   map[string][]int{"postgres/bin/postgres": {1004}},
		cmdlines: map[int][]string{
			1004: {"/home/yugabyte/tserver/postgres/bin/postgres", "-D", "/home/yugabyte/pgdata"},
			1006: {"postgres: YSQL webserver   "},
			2002: {"/usr/lib/postgresql/16/bin/postgres", "-D", "/var/lib/postgresql/data"},
			2003: {"postgres: background writer   "},
		},
		ppids: map[int]int{1006: 1004, 2003: 2002},
		ssOutput: `LISTEN 0 128 127.0.0.1:5433 0.0.0.0:* users:(("postgres",pid=1004,fd=1))
LISTEN 0 128 0.0.0.0:13000 0.0.0.0:* users:(("postgres",pid=1006,fd=5))
LISTEN 0 128 127.0.0.1:15432 0.0.0.0:* users:(("postgres",pid=2003,fd=5))
`,
	}
	res, err := NewDetectorWithSystem(sys).Detect(context.Background())
	if err != nil {
		t.Fatalf("Detect() error = %v", err)
	}
	postgres := statusForRole(res, pb.YugabyteProcessRole_YB_ROLE_POSTGRES)
	if postgres == nil || !equalPorts(postgres.GetListenPorts(), []uint32{5433, 13000}) {
		t.Fatalf("postgres listen ports = %v, want [5433 13000]", postgres.GetListenPorts())
	}
	// A child of an unverified (system) postgres must stay unattributed.
	for _, listener := range res.GetOtherListeners() {
		if listener.GetPid() == 2003 {
			t.Errorf("child of unverified parent must not be attributed, got %+v", listener)
		}
	}
}

func TestDeriveFlagfilePath(t *testing.T) {
	got := deriveFlagfilePath("/home/yugabyte/master/bin/yb-master")
	want := "/home/yugabyte/master/conf/server.conf"
	if got != want {
		t.Errorf("deriveFlagfilePath() = %q, want %q", got, want)
	}
	if deriveFlagfilePath("") != "" {
		t.Errorf("deriveFlagfilePath(\"\") should be empty")
	}
}

func equalPorts(got, want []uint32) bool {
	if len(got) != len(want) {
		return false
	}
	for i := range want {
		if got[i] != want[i] {
			return false
		}
	}
	return true
}

func hasArg(args []*pb.ProcessArg, key, value string) bool {
	for _, arg := range args {
		if arg.GetKey() == key && arg.GetValue() == value {
			return true
		}
	}
	return false
}

func hasListenerPort(
	listeners []*pb.ProcessListener,
	port uint32,
	role pb.YugabyteProcessRole,
) bool {
	for _, listener := range listeners {
		if listener.GetPort() == port && listener.GetRole() == role {
			return true
		}
	}
	return false
}
