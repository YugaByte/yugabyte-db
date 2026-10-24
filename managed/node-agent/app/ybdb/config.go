// Copyright (c) YugabyteDB, Inc.

package ybdb

import (
	pb "node-agent/generated/service"
	"sort"
	"strings"
)

const (
	// maxFlagfileBytes caps the number of bytes read from a flagfile.
	maxFlagfileBytes = 256 * 1024
	// maxConfigKeys caps the number of arguments returned per source to avoid
	// excessively large responses.
	maxConfigKeys = 500
	// redactedValue replaces the value of sensitive arguments.
	redactedValue = "REDACTED"
	// flagfileKey is the argument key that points to the flagfile itself.
	flagfileKey = "flagfile"
)

// argKV is an intermediate key-value representation of a process argument.
type argKV struct {
	key   string
	value string
}

// sensitiveKeySubstrings are matched (case-insensitively) against argument keys
// to decide whether the value must be redacted.
var sensitiveKeySubstrings = []string{
	"password",
	"secret",
	"token",
	"jwt",
	"credential",
	"passphrase",
}

// isSensitiveKey returns true when the value for the key must be redacted.
func isSensitiveKey(key string) bool {
	lower := strings.ToLower(key)
	for _, substr := range sensitiveKeySubstrings {
		if strings.Contains(lower, substr) {
			return true
		}
	}
	return false
}

// mergeArgs overlays the command line arguments on top of the flagfile
// arguments, matching gflags precedence where the command line wins. The
// flagfile pointer itself is excluded from the effective set.
func mergeArgs(flagfileArgs, cmdArgs []argKV) []argKV {
	merged := make(map[string]string)
	for _, arg := range flagfileArgs {
		merged[arg.key] = arg.value
	}
	for _, arg := range cmdArgs {
		if arg.key == flagfileKey {
			continue
		}
		merged[arg.key] = arg.value
	}
	return sortedArgs(merged)
}

// sortedArgs returns the map as a slice sorted by key for deterministic output.
func sortedArgs(m map[string]string) []argKV {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	args := make([]argKV, 0, len(keys))
	for _, k := range keys {
		args = append(args, argKV{key: k, value: m[k]})
	}
	return args
}

// toProtoArgs converts the arguments to their proto form, capping the count and
// redacting sensitive values.
func toProtoArgs(args []argKV, source string) []*pb.ProcessArg {
	if len(args) > maxConfigKeys {
		args = args[:maxConfigKeys]
	}
	result := make([]*pb.ProcessArg, 0, len(args))
	for _, arg := range args {
		value := arg.value
		if isSensitiveKey(arg.key) {
			value = redactedValue
		}
		result = append(result, &pb.ProcessArg{
			Key:    arg.key,
			Value:  value,
			Source: source,
		})
	}
	return result
}

// buildConfig assembles the ProcessConfig from the parsed command line and the
// flagfile content when it is readable.
func buildConfig(
	flagfilePath string,
	flagfileExists bool,
	flagfileReadable bool,
	cmdArgs []argKV,
	flagfileContent []byte,
) *pb.ProcessConfig {
	sortedCmdArgs := sortArgSlice(cmdArgs)
	var flagfileArgs []argKV
	if flagfileReadable {
		flagfileArgs = parseFlagfile(flagfileContent)
	}
	sortedFlagfileArgs := sortArgSlice(flagfileArgs)
	mergedArgs := mergeArgs(sortedFlagfileArgs, sortedCmdArgs)
	return &pb.ProcessConfig{
		FlagfilePath:     flagfilePath,
		FlagfileExists:   flagfileExists,
		FlagfileReadable: flagfileReadable,
		CmdlineArgs:      toProtoArgs(sortedCmdArgs, "cmdline"),
		FlagfileArgs:     toProtoArgs(sortedFlagfileArgs, "flagfile"),
		Args:             toProtoArgs(mergedArgs, "merged"),
	}
}

// sortArgSlice returns a copy of the argument slice sorted by key.
func sortArgSlice(args []argKV) []argKV {
	sorted := make([]argKV, len(args))
	copy(sorted, args)
	sort.SliceStable(sorted, func(i, j int) bool {
		return sorted[i].key < sorted[j].key
	})
	return sorted
}
