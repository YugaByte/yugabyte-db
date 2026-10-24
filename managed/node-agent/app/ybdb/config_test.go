// Copyright (c) YugabyteDB, Inc.

package ybdb

import (
	pb "node-agent/generated/service"
	"testing"
)

func TestIsSensitiveKey(t *testing.T) {
	sensitive := []string{
		"ysql_hba_conf_csv_password",
		"webserver_password",
		"my_secret",
		"api_token",
		"jwt",
		"ldap_bind_credential",
		"tls_passphrase",
	}
	for _, key := range sensitive {
		if !isSensitiveKey(key) {
			t.Errorf("isSensitiveKey(%q) = false, want true", key)
		}
	}
	nonSensitive := []string{"rpc_bind_addresses", "webserver_port", "placement_cloud"}
	for _, key := range nonSensitive {
		if isSensitiveKey(key) {
			t.Errorf("isSensitiveKey(%q) = true, want false", key)
		}
	}
}

func TestMergeArgs(t *testing.T) {
	flagfileArgs := []argKV{
		{key: "webserver_port", value: "7000"},
		{key: "rpc_bind_addresses", value: "10.0.0.1:7100"},
	}
	cmdArgs := []argKV{
		{key: "flagfile", value: "/conf/server.conf"},
		{key: "webserver_port", value: "7001"},
	}
	got := mergeArgs(flagfileArgs, cmdArgs)
	want := []argKV{
		{key: "rpc_bind_addresses", value: "10.0.0.1:7100"},
		// Command line wins over the flagfile value.
		{key: "webserver_port", value: "7001"},
	}
	if len(got) != len(want) {
		t.Fatalf("mergeArgs() len = %d, want %d (%+v)", len(got), len(want), got)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("mergeArgs()[%d] = %+v, want %+v", i, got[i], want[i])
		}
	}
}

func TestBuildConfigRedactsAllSources(t *testing.T) {
	cmdArgs := []argKV{
		{key: "flagfile", value: "/conf/server.conf"},
		{key: "ysql_password", value: "supersecret"},
	}
	flagfileContent := []byte("--webserver_port=7000\n--ldap_bind_password=topsecret\n")
	config := buildConfig(
		"/conf/server.conf",
		true, /* exists */
		true, /* readable */
		cmdArgs,
		flagfileContent,
	)
	if config.GetFlagfilePath() != "/conf/server.conf" {
		t.Errorf("flagfile path = %q", config.GetFlagfilePath())
	}
	if !config.GetFlagfileExists() || !config.GetFlagfileReadable() {
		t.Errorf("expected flagfile to exist and be readable")
	}
	assertRedacted(t, config.GetCmdlineArgs(), "ysql_password")
	assertRedacted(t, config.GetFlagfileArgs(), "ldap_bind_password")
	assertRedacted(t, config.GetArgs(), "ysql_password")
	assertRedacted(t, config.GetArgs(), "ldap_bind_password")
}

func TestBuildConfigUnreadableFlagfile(t *testing.T) {
	cmdArgs := []argKV{{key: "flagfile", value: "/conf/server.conf"}}
	config := buildConfig(
		"/conf/server.conf",
		true,  /* exists */
		false, /* readable */
		cmdArgs,
		nil,
	)
	if len(config.GetFlagfileArgs()) != 0 {
		t.Errorf("expected no flagfile args when unreadable, got %+v", config.GetFlagfileArgs())
	}
	if config.GetFlagfileReadable() {
		t.Errorf("expected flagfile readable to be false")
	}
}

func assertRedacted(t *testing.T, args []*pb.ProcessArg, key string) {
	t.Helper()
	for _, arg := range args {
		if arg.GetKey() == key {
			if arg.GetValue() != redactedValue {
				t.Errorf("expected key %q to be redacted, got %q", key, arg.GetValue())
			}
			return
		}
	}
	t.Errorf("key %q not found in args", key)
}
