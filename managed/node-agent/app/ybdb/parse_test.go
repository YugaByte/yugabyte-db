// Copyright (c) YugabyteDB, Inc.

package ybdb

import (
	"reflect"
	"testing"
)

func TestParseSsListeners(t *testing.T) {
	output := `LISTEN 0 4096 0.0.0.0:7000 0.0.0.0:* users:(("yb-master",pid=1001,fd=370))
LISTEN 0 4096 [::]:7100 [::]:* users:(("yb-master",pid=1001,fd=12))
LISTEN 0 128 127.0.0.1:5433 0.0.0.0:*
LISTEN 0 128 *:9300 *:*
garbage line
`
	got := parseSsListeners(output)
	want := []ssListener{
		{port: 7000, protocol: "tcp", pid: 1001, processName: "yb-master"},
		{port: 7100, protocol: "tcp6", pid: 1001, processName: "yb-master"},
		{port: 5433, protocol: "tcp"},
		{port: 9300, protocol: "tcp"},
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("parseSsListeners() = %+v, want %+v", got, want)
	}
}

func TestParsePpid(t *testing.T) {
	tests := []struct {
		stat string
		ppid int
		ok   bool
	}{
		{"1006 (postgres) S 1004 1004 1004 0 -1", 1004, true},
		{"1007 (weird ) name)) R 42 1 1 0", 42, true},
		{"1008 (truncated", 0, false},
		{"1009 (postgres) S", 0, false},
		{"1010 (postgres) S notanumber", 0, false},
	}
	for _, tt := range tests {
		ppid, ok := parsePpid(tt.stat)
		if ppid != tt.ppid || ok != tt.ok {
			t.Errorf("parsePpid(%q) = (%d, %t), want (%d, %t)",
				tt.stat, ppid, ok, tt.ppid, tt.ok)
		}
	}
}

func TestPortFromAddress(t *testing.T) {
	tests := []struct {
		addr string
		port uint32
		ok   bool
	}{
		{"0.0.0.0:7000", 7000, true},
		{"[::]:7100", 7100, true},
		{"*:9300", 9300, true},
		{"127.0.0.1:5433", 5433, true},
		{"no-port", 0, false},
		{"trailing:", 0, false},
	}
	for _, tt := range tests {
		port, ok := portFromAddress(tt.addr)
		if port != tt.port || ok != tt.ok {
			t.Errorf("portFromAddress(%q) = (%d, %t), want (%d, %t)",
				tt.addr, port, ok, tt.port, tt.ok)
		}
	}
}

func TestParseCmdline(t *testing.T) {
	tests := []struct {
		name         string
		args         []string
		wantFlagfile string
		wantArgs     []argKV
	}{
		{
			name: "flagfile space separated",
			args: []string{
				"/home/yugabyte/tserver/bin/yb-tserver",
				"--flagfile",
				"/home/yugabyte/tserver/conf/server.conf",
			},
			wantFlagfile: "/home/yugabyte/tserver/conf/server.conf",
			wantArgs: []argKV{
				{key: "flagfile", value: "/home/yugabyte/tserver/conf/server.conf"},
			},
		},
		{
			name: "flagfile equals and extra flags",
			args: []string{
				"/bin/yb-master",
				"--flagfile=/conf/server.conf",
				"--webserver_port=7000",
				"--use_client_to_server_encryption",
			},
			wantFlagfile: "/conf/server.conf",
			wantArgs: []argKV{
				{key: "flagfile", value: "/conf/server.conf"},
				{key: "webserver_port", value: "7000"},
				{key: "use_client_to_server_encryption", value: ""},
			},
		},
		{
			name:         "no flagfile",
			args:         []string{"/bin/yb-tserver", "--rpc_bind_addresses", "10.0.0.1:9100"},
			wantFlagfile: "",
			wantArgs:     []argKV{{key: "rpc_bind_addresses", value: "10.0.0.1:9100"}},
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			flagfile, args := parseCmdline(tt.args)
			if flagfile != tt.wantFlagfile {
				t.Errorf("flagfile = %q, want %q", flagfile, tt.wantFlagfile)
			}
			if !reflect.DeepEqual(args, tt.wantArgs) {
				t.Errorf("args = %+v, want %+v", args, tt.wantArgs)
			}
		})
	}
}

func TestParseFlagfile(t *testing.T) {
	content := []byte(`# This is a comment

--rpc_bind_addresses=10.0.0.1:7100
--webserver_port=7000
placement_cloud=aws
--use_node_to_node_encryption
`)
	got := parseFlagfile(content)
	want := []argKV{
		{key: "rpc_bind_addresses", value: "10.0.0.1:7100"},
		{key: "webserver_port", value: "7000"},
		{key: "placement_cloud", value: "aws"},
		{key: "use_node_to_node_encryption", value: ""},
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("parseFlagfile() = %+v, want %+v", got, want)
	}
}
