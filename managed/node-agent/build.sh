#!/usr/bin/env bash
# Copyright (c) YugabyteDB, Inc.

set -euo pipefail

export GO111MODULE=on

# https://protobuf.dev/support/version-support/
readonly protoc_version=33.0
readonly package_name='node-agent'
readonly default_platforms=("linux/amd64" "linux/arm64")
readonly skip_dirs=("third-party" "proto" "generated" "build" "resources" "ybops" "target")

readonly base_dir=$(dirname "$0")
pushd "$base_dir"
readonly project_dir=$(pwd)
popd

export PROJECT_DIR="$project_dir"
export GOPATH=$project_dir/third-party
export GOBIN=$GOPATH/bin
export PATH=$GOBIN:$PATH
export GO_LDFLAGS="-w"
mkdir -p "$GOBIN"

readonly build_output_dir="${project_dir}/build"
if [[ ! -d $build_output_dir ]]; then
    mkdir "$build_output_dir"
fi
readonly grpc_output_dir="${project_dir}/generated"
# Python does not support package option for protobuf.
readonly grpc_python_output_dir="${grpc_output_dir}/ybops/node_agent"
readonly grpc_proto_dir="${project_dir}/proto"
readonly grpc_proto_files="${grpc_proto_dir}"/*.proto

to_lower() {
  out=$(awk '{print tolower($0)}' <<< "$1")
  echo "$out"
}

readonly build_os=$(to_lower "$(uname -s)")
readonly build_arch=$(to_lower "$(uname -m)")

setup_protoc() {
    install_protoc=false
    if [ ! -f "$GOBIN"/protoc ]; then
        install_protoc=true
    else
        installed_version=$("$GOBIN"/protoc --version | awk -F ' ' '{print $2}')
        if [ "$installed_version" != "$protoc_version" ]; then
            install_protoc=true
        fi
    fi
    if [ "$install_protoc" = true ]; then
        pushd "$project_dir"
        go install google.golang.org/protobuf/cmd/protoc-gen-go@v1.36.10
        go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@v1.6.0
        local release_url=https://github.com/protocolbuffers/protobuf/releases
        local protoc_os=$build_os
        if [ "$protoc_os" = "darwin" ]; then
            protoc_os=osx
        fi
        local protoc_arch=$build_arch
        if [ "$protoc_arch" = "arm64" ]; then
            protoc_arch=aarch_64
        fi
        popd
        local protoc_filename=protoc-${protoc_version}-${protoc_os}-${protoc_arch}.zip
        pushd "$GOPATH"
        curl -fsSLO ${release_url}/download/v"${protoc_version}"/"${protoc_filename}"
        unzip -o "$protoc_filename" -d tmp
        cp -rf tmp/bin/protoc "$GOBIN"/protoc
        rm -rf tmp "$protoc_filename"
        popd
    fi
    which protoc
    protoc --version
    which protoc-gen-go
    protoc-gen-go --version
    which protoc-gen-go-grpc
    protoc-gen-go-grpc --version
}

generate_golang_grpc_files() {
    if [[ ! -d "$grpc_output_dir" ]]; then
        mkdir -p "$grpc_output_dir"
    fi
    protoc -I"$grpc_proto_dir" --go_out="$grpc_output_dir" --go-grpc_out="$grpc_output_dir"  \
    --go-grpc_opt=require_unimplemented_servers=false $grpc_proto_files
}

build_pymodule() {
    if [[ ! -d "$grpc_python_output_dir" ]]; then
        mkdir -p "$grpc_python_output_dir"
    fi
    pushd "$grpc_output_dir"
    python3 -m grpc_tools.protoc -I"$grpc_proto_dir" --python_out="$grpc_python_output_dir" \
    --grpc_python_out="$grpc_python_output_dir" $grpc_proto_files
    # Python does not support custom package. Workaround to change the package name.
    if [ "$build_os" = "darwin" ]; then
        sed -i "" -e 's/^import \(.*_pb2.*\) as/from ybops.node_agent import \1 as/' \
        "$grpc_python_output_dir"/*.py
    else
        sed -i -e 's/^import \(.*_pb2.*\) as/from ybops.node_agent import \1 as/' \
        "$grpc_python_output_dir"/*.py
    fi
    cp -rf ../ybops "$grpc_output_dir"/
    popd
}

get_executable_name(){
    local num_args="$#"
    local os=$1
    local arch=$2
    local suffix=""
    if [ "$num_args" -gt 2 ]; then
        suffix="-$3"
    fi
    executable=${package_name}${suffix}-${os}-${arch}
    if [ "$os" = "windows" ]; then
        executable+='.exe'
    fi
    echo "$executable"
}

get_node_agent_executable_name(){
    local os=$1
    local arch=$2
    local name=$(get_executable_name "$os" "$arch")
    echo "$name"
}

get_ynp_executable_name(){
    local os=$1
    local arch=$2
    local name=$(get_executable_name "$os" "$arch" "ynp")
    echo "$name"
}

stage_shared_templates() {
    # gcp-fed-creds.json.j2 is owned by YBA (managed/src/main/resources/federation) so YBA and
    # node-agent render the same external_account credential from one source. Stage YBA's copy into
    # the node-agent templates (this file is git-ignored under resources/templates/server).
    local shared_src="$project_dir/../src/main/resources/federation/gcp-fed-creds.json.j2"
    local dest_dir="$project_dir/resources/templates/server"
    if [ -f "$shared_src" ]; then
        mkdir -p "$dest_dir"
        cp -f "$shared_src" "$dest_dir/gcp-fed-creds.json.j2"
    else
        echo "WARN: shared template not found at $shared_src"
    fi
}

prepare() {
    setup_protoc
    generate_golang_grpc_files
}

# The FIPS 140-3 validated build of the Go Cryptographic Module: v1.0.0 is CMVP certificate
# #5247. Building with it links that module and defaults the binary to GODEBUG=fips140=on, which
# "go version -m <binary>" records. Overridable so a build can be pointed at a newer module once
# one is certified, but the default is the certified one - not the newest.
GOFIPS140_VERSION="${GOFIPS140_VERSION:-v1.0.0}"
GO_LDFLAGS_ARGS=()
if [[ -n "$GO_LDFLAGS" ]]; then
    GO_LDFLAGS_ARGS=(-ldflags "$GO_LDFLAGS")
fi

build_ynp_go() {
    local exec_name=$(get_ynp_executable_name "$os" "$arch")
    local executable="$build_output_dir/$exec_name"
    pushd "$project_dir"
    echo "Building ${exec_name}"
    env GOOS="$os" GOARCH="$arch" CGO_ENABLED=0 GOFIPS140="$GOFIPS140_VERSION" \
    go build "${GO_LDFLAGS_ARGS[@]}" -o "$executable" "$project_dir"/ynp/cmd/main.go
    if [ $? -ne 0 ]; then
        echo "Build failed for $exec_name"
        exit 1
    fi
    popd
}

build_for_platform() {
    local os=$1
    local arch=$2
    build_ynp_go
    local exec_name=$(get_node_agent_executable_name "$os" "$arch")
    local executable="$build_output_dir/$exec_name"
    pushd "$project_dir"
    echo "Building ${exec_name}"
    env GOOS="$os" GOARCH="$arch" CGO_ENABLED=0 GOFIPS140="$GOFIPS140_VERSION" \
    go build "${GO_LDFLAGS_ARGS[@]}" -o "$executable" "$project_dir"/cmd/cli/main.go
    if [ $? -ne 0 ]; then
        echo "Build failed for $exec_name"
        exit 1
    fi
    popd
}

build_for_platforms() {
    for platform in "$@"
    do
        platform_split=(${platform//\// })
        local os=${platform_split[0]}
        local arch=${platform_split[1]}
        build_for_platform "$os" "$arch"
    done
}

clean_build() {
    rm -rf "$build_output_dir"
    rm -rf "$grpc_output_dir"
}



format() {
    pushd "$project_dir"
    go install github.com/segmentio/golines@v0.13.0
    go install golang.org/x/tools/cmd/goimports@v0.24.0
    for dir in */ ; do
        # Remove trailing slash.
        dir=$(echo "${dir}" | sed 's/\/$//')
        if [[ "${skip_dirs[@]}" =~ "${dir}" ]]; then
            continue
        fi
        golines -m 100 -t 4 -w ./$dir/.
    done
    popd
}

run_test() {
    local dir=$1
    pushd "$project_dir"
    echo "Running tests in ${dir}..."
    set +e
    go clean -testcache
    mkdir -p "target/test-reports/${dir}"
    local json_file="target/test-reports/${dir}/tmp_results.json"
    local xml_file="target/test-reports/${dir}/node_agent_test_results.xml"
    go test -json -short --tags testonly ./"$dir"/... > "$json_file"
    if [ -s "$json_file" ]; then
        go-junit-report -parser gojson -set-exit-code -iocopy -out "$xml_file" < "$json_file"
    fi
    status=$?
    set -e
    popd
    return $status
}

run_tests() {
    local testone_path="${1-}"
    # Run all tests if one fails.
    local failed_tests=()
    go install github.com/jstemmer/go-junit-report/v2@v2.1.0
    pushd "$project_dir"
    mkdir -p target/test-reports
    if [ -n "$testone_path" ]; then
        # E.g testone app/task/module.
        local test_files=$(find "${testone_path}" -name '*_test.go')
        for test_file in $test_files; do
            local test_dir=$(dirname "$test_file")
            run_test "$test_dir"
        done
    else
        for dir in */ ; do
            # Remove trailing slash.
            dir=$(echo "${dir}" | sed 's/\/$//')
            if [[ "${skip_dirs[@]}" =~ "${dir}" ]]; then
                echo "Skipping directory ${dir}"
                continue
            fi
            if [ -n "$testone_path" ] && [ "$dir" != "$testone_path" ]; then
                echo "Skipping directory ${dir} for testone"
                continue
            fi
            run_test "$dir"
            if [ $? -ne 0 ]; then
                failed_tests+=("$dir")
            fi
        done
    fi
    popd
    if [ ${#failed_tests[@]} -ne 0 ]; then
        echo "Failed tests: ${failed_tests[*]}"
        exit 1
    else
        echo "All tests passed."
    fi
}

package_for_platform() {
    local os=$1
    local arch=$2
    local version=$3
    staging_dir_name="node_agent-${version}-${os}-${arch}"
    version_dir="${build_output_dir}/${staging_dir_name}/${version}"
    script_dir="${version_dir}/scripts"
    bin_dir="${version_dir}/bin"
    templates_dir="${version_dir}/templates"
    echo "Packaging ${staging_dir_name}"
    node_agent_exec_name=$(get_node_agent_executable_name "$os" "$arch")
    ynp_exec_name=$(get_ynp_executable_name "$os" "$arch")
    pushd "$build_output_dir"
    echo "Creating staging directory ${staging_dir_name}"
    rm -rf "$staging_dir_name"
    mkdir "$staging_dir_name"
    mkdir -p "$script_dir"
    mkdir -p "$bin_dir"
    mkdir -p "$templates_dir"
    cp -rf "$node_agent_exec_name" "${bin_dir}/node-agent"
    cp -rf "$ynp_exec_name" "${bin_dir}/node-provisioner"
    # Follow the symlinks.
    cp -Lf ../version.txt "${version_dir}"/version.txt
    cp -Lf ../version_metadata.json "${version_dir}"/version_metadata.json
    # Stage YBA's canonical gcp-fed-creds.json.j2 into resources/templates/server so the copy below
    # picks it up. Only the package needs it (the node renders it at runtime); the build/test flows
    # don't, so this is here rather than in prepare().
    stage_shared_templates
    pushd "$project_dir/resources"
    cp -rf templates/* "$templates_dir/"
    cp -rf preflight_check.sh "${script_dir}"/preflight_check.sh
    cp -rf node-agent-installer.sh "${bin_dir}"/node-agent-installer.sh
    cp -rf ynp "${script_dir}"/ynp
    cp -rf node-agent-provision.sh "${script_dir}"/node-agent-provision.sh
    cp -rf earlyoom-installer.sh "${script_dir}"/earlyoom-installer.sh
    cp -rf configure_earlyoom_service.sh "${script_dir}"/configure_earlyoom_service.sh
    cp -rf node-agent-provision.yaml "${script_dir}"/node-agent-provision.yaml
    cp -rf templates/server/* "${script_dir}"/ynp/modules/provision/systemd/templates/
    cp -rf templates/server/* "${script_dir}"/ynp/modules/provision/rootsystemd/templates/
    chmod 755 "${script_dir}"/*.sh
    chmod 755 "${bin_dir}"/*.sh
    popd
    # Create the tar.gz package without ./ prefix.
    tar -zcf "${staging_dir_name}.tar.gz" -C "$staging_dir_name" "$version"
    popd
}

package_for_platforms() {
  local version=$1
  shift
  for platform in "$@"
  do
      platform_split=(${platform//\// })
      os=${platform_split[0]}
      arch=${platform_split[1]}
      package_for_platform $os $arch $version
  done
}

update_dependencies() {
    pushd "$project_dir"
    go mod tidy -e
    popd
}

# Initialize the vars.
fmt=false
prepare=false
build=false
clean=false
test=false
testone=false
package=false
version=0
update_dependencies=false
build_pymodule=false
testone_path=""


show_help() {
    cat >&2 <<-EOT

Usage:
./build.sh <fmt|prepare|build|clean|test|testone <package>|package <version>|update-dependencies|build-pymodule>
EOT
exit 1
}

while [[ $# -gt 0 ]]; do
  case $1 in
    help)
      show_help >&2
      ;;
    fmt)
      fmt=true
      ;;
    prepare)
      prepare=true
      ;;
    build)
      build=true
      ;;
    clean)
      clean=true
      ;;
    test)
      test=true
      ;;
    testone)
      testone=true
      shift
      if [[ $# -gt 0 ]]; then
        testone_path=$1
      fi
      ;;
    package)
      package=true
      shift
      # Read the args to package.
      if [[ $# -gt 0 ]]; then
        version=$1
      fi
      ;;
    update-dependencies)
      update_dependencies=true
      ;;
    build-pymodule)
      build_pymodule=true
      ;;
    *)
      echo "Unknown option $1"
      exit 1
      ;;
  esac
  if [[ $# -gt 0 ]]; then
    shift
  fi
done

NODE_AGENT_PLATFORMS="${NODE_AGENT_PLATFORMS:=}"
# Release build passes the platforms in the environment.
if [ -z "${NODE_AGENT_PLATFORMS}" ]; then
    PLATFORMS=("${default_platforms[@]}")
    echo "Using default platforms ${PLATFORMS[@]}"
else
    PLATFORMS=($(echo "${NODE_AGENT_PLATFORMS}"))
    echo "Using environment platforms ${PLATFORMS[@]}"
fi

help_needed=true
if [ "$clean" == "true" ]; then
   help_needed=false
   echo "Cleaning up..."
   clean_build
fi

if [ "$update_dependencies" == "true" ]; then
    help_needed=false
    echo "Updating dependencies..."
    update_dependencies
fi

if [ "$fmt" == "true" ]; then
    help_needed=false
    echo "Formatting..."
    format
fi

if [ "$prepare" == "true" ]; then
    help_needed=false
    echo "Preparing..."
    prepare
fi

if [ "$build" == "true" ]; then
    help_needed=false
    echo "Building..."
    pushd "$project_dir"
    if [ ! -f "go.mod" ]; then
        go mod init node-agent
    fi
    popd
    format
    prepare
    build_for_platforms "${PLATFORMS[@]}"
fi

if [ "$test" == "true" ]; then
    help_needed=false
    echo "Running tests..."
    prepare
    run_tests
fi

if [ "$testone" == "true" ]; then
    if [ -z "$testone_path"  ]; then
        echo "Test path is not specified for testone"
        exit 1
    fi
    help_needed=false
    echo "Running test for ${testone_path}..."
    prepare
    run_tests "$testone_path"
fi

if [ "$package" == "true" ]; then
    if [ -z "$version"  ]; then
        echo "Version is not specified"
    else
        help_needed=false
        echo "Packaging..."
        package_for_platforms "$version" "${PLATFORMS[@]}"
    fi
fi

if [ "$build_pymodule" == "true" ]; then
    help_needed=false
    echo "Installing python module..."
    build_pymodule
fi

if [ "$help_needed" == "true" ]; then
    show_help
fi
