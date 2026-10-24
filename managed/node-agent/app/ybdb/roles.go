// Copyright (c) YugabyteDB, Inc.

// Package ybdb provides read-only detection of running YugabyteDB related
// processes on a node, the ports they listen on and the configuration
// (flagfile and command line arguments) they were started with.
package ybdb

import (
	pb "node-agent/generated/service"
	"strings"
)

// roleDef describes how a YugabyteDB related process is detected.
type roleDef struct {
	role pb.YugabyteProcessRole
	// name is a short human readable name used in logs.
	name string
	// pgrepPattern is passed to "pgrep -f" to find candidate pids.
	pgrepPattern string
	// classifiers are substrings matched against a process command line to
	// confirm the role. The first matching role wins.
	classifiers []string
	// defaultPorts are the well known ports for the role.
	defaultPorts []uint32
	// hasConfig indicates whether the flagfile and arguments are extracted.
	hasConfig bool
}

// roleDefs is the ordered list of roles that are detected in v1. The order is
// significant because it is used to classify a process by its command line and
// the first match wins (for example, yb-tserver is matched before postgres so
// that the tserver process is not misclassified).
var roleDefs = []roleDef{
	{
		role:         pb.YugabyteProcessRole_YB_ROLE_MASTER,
		name:         "master",
		pgrepPattern: "bin/yb-master",
		classifiers:  []string{"/bin/yb-master"},
		defaultPorts: []uint32{7000, 7100},
		hasConfig:    true,
	},
	{
		role:         pb.YugabyteProcessRole_YB_ROLE_TSERVER,
		name:         "tserver",
		pgrepPattern: "bin/yb-tserver",
		classifiers:  []string{"/bin/yb-tserver"},
		defaultPorts: []uint32{9000, 9100},
		hasConfig:    true,
	},
	{
		role:         pb.YugabyteProcessRole_YB_ROLE_CONTROLLER,
		name:         "controller",
		pgrepPattern: "bin/yb-controller-server",
		classifiers:  []string{"yb-controller-server"},
		defaultPorts: []uint32{14000, 18018},
		hasConfig:    true,
	},
	{
		role:         pb.YugabyteProcessRole_YB_ROLE_POSTGRES,
		name:         "postgres",
		pgrepPattern: "postgres/bin/postgres",
		classifiers:  []string{"postgres/bin/postgres", "yb-postmaster"},
		defaultPorts: []uint32{5433, 13000},
		hasConfig:    false,
	},
	{
		role:         pb.YugabyteProcessRole_YB_ROLE_NODE_EXPORTER,
		name:         "node_exporter",
		pgrepPattern: "node_exporter",
		classifiers:  []string{"node_exporter"},
		defaultPorts: []uint32{9300},
		hasConfig:    false,
	},
}

// classifyRole returns the role for the given process command line, or
// YB_ROLE_UNKNOWN when it does not match any known YugabyteDB process.
func classifyRole(args []string) pb.YugabyteProcessRole {
	if len(args) == 0 {
		return pb.YugabyteProcessRole_YB_ROLE_UNKNOWN
	}
	exe := args[0]
	for _, def := range roleDefs {
		for _, classifier := range def.classifiers {
			if strings.Contains(exe, classifier) {
				return def.role
			}
			if strings.HasPrefix(classifier, "/") && strings.Contains(exe, classifier[1:]) {
				return def.role
			}
		}
	}
	return pb.YugabyteProcessRole_YB_ROLE_UNKNOWN
}
