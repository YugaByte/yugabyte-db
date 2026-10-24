#!/usr/bin/env bash

root_certs_generation=false
node_certs_generation=false
key_generation=false
root_certs_path=""
server_certs_path=""
temp_certs_path=""
key_path=""
keyname=""
hostname=""
hostname_type=""

show_help() {
  cat >&1 <<-EOT
openssl_proxy.sh is to be used to create SSL certificates for secure deployment of YugabyteDB.
---------------------------------------------------------------------------------------------------
Usage: ${0##*/} [<action>] [<flags>]

Actions:
    generate-ca
        Generate root certificates to be used for generation of each node's certificates

    generate-server-cert
        Generate node server certificates.

    generate-key
        Generate SSL key.

Flags:

    --help, -h
        Show help.

    --root-ca-path, --rcp, -rcp
        Path where root certs needs to be generated or where root certs are present (in case of \
node certs generation)

    --server-cert-path, --scp, -scp
        Path where the server certificates will be generated

    --hostname, --hn, -hn
        Value of node's commonName to be set in certificates. Needs to be same as the
        advertise address of the node

    --key-path, --kp, -kp
        Path where the key will be generated.

    --keyname, --kn, -kn
        Name of the key to be generated.

    --hostname-type, --ht, -ht
        Hostname type. Accepted vales: IP/DNS
---------------------------------------------------------------------------------------------------
EOT
}

generate_root_certs() {
    mkdir -p "$root_certs_path"

    echo '# CA root configuration file
    [ ca ]
    default_ca = yugabyted_root_ca

    [ yugabyted_root_ca ]
    default_days = 1460
    serial = '"$root_certs_path"'/serial.txt
    database = '"$root_certs_path"'/index.txt
    default_md = sha256
    policy = yugabyted_policy

    [ yugabyted_policy ]
    organizationName = supplied
    commonName = supplied

    [req]
    prompt=no
    distinguished_name = YugabyteDB
    x509_extensions = YugabyteDB_extensions

    [ YugabyteDB ]
    organizationName = Yugabyte
    commonName = Root CA for YugabyteDB

    [ YugabyteDB_extensions ]
    keyUsage = critical,digitalSignature,nonRepudiation,keyEncipherment,keyCertSign
    basicConstraints = critical,CA:true,pathlen:1' > "$root_certs_path"/ca.conf

    touch "$root_certs_path"/index.txt
    echo '01' > "$root_certs_path"/serial.txt
    openssl genrsa -out "$root_certs_path"/ca.key
    chmod 400 "$root_certs_path"/ca.key
    openssl req -new -x509 -config "$root_certs_path"/ca.conf \
                -key "$root_certs_path"/ca.key \
                -out "$root_certs_path"/ca.crt \
                -days 1460
}

generate_node_certs() {
    mkdir -p "$temp_certs_path"

    cp "$root_certs_path"/ca.crt "$temp_certs_path"/

    echo '# Example node configuration file
    [ req ]
    prompt=no
    distinguished_name = YugabyteDB_Node

    [ YugabyteDB_Node ]
    organizationName = Yugabyte
    commonName = '"$hostname"'

    [ req_ext ]
    subjectAltName = @alt_names
    [alt_names]' > "$temp_certs_path"/node.conf

    if [[ $hostname_type == "DNS" ]]; then
        echo 'DNS.1 = '"$hostname"'' >> "$temp_certs_path"/node.conf
    else
        echo 'IP.1 = '"$hostname"'' >> "$temp_certs_path"/node.conf
    fi

    openssl genrsa -out "$temp_certs_path"/node."$hostname".key
    chmod 400 "$temp_certs_path"/node."$hostname".key

    openssl req -new -config "$temp_certs_path"/node.conf \
                -key "$temp_certs_path"/node."$hostname".key \
                -out "$temp_certs_path"/node.csr


    openssl ca -config "$root_certs_path"/ca.conf \
                -keyfile "$root_certs_path"/ca.key \
                -cert "$root_certs_path"/ca.crt \
                -policy yugabyted_policy \
                -out "$temp_certs_path"/node."$hostname".crt \
                -outdir "$temp_certs_path" \
                -in "$temp_certs_path"/node.csr \
                -days 365 \
                -batch \
                -extfile "$temp_certs_path"/node.conf \
                -extensions req_ext

    cp "$temp_certs_path"/ca.crt \
        "$temp_certs_path"/node."$hostname".key \
        "$temp_certs_path"/node."$hostname".crt \
        "$server_certs_path"

    rm -rf "$temp_certs_path"
}

generate_key() {
    mkdir -p "$key_path"

    openssl rand -out "$key_path/${keyname}.key" 32
}

while [[ $# -gt 0 ]]; do
    case ${1//_/-} in
    -h|--help)
      show_help >&1
      exit 1
    ;;
    generate-ca)
        root_certs_generation=true
    ;;
    --root-ca-path|--rcp|-rcp)
        root_certs_path="$2"
        shift
    ;;
    generate-server-cert)
        node_certs_generation=true
    ;;
    --server-certs-path|--scp|-scp)
        server_certs_path="$2"
        shift
    ;;
    --hostname|--hn|-hn)
        hostname="$2"
        shift
    ;;
    --hostname_type|--ht|-ht)
        hostname_type="$2"
        shift
    ;;
    generate-key)
        key_generation=true
    ;;
    --key-path|--kp|-kp)
        key_path="$2"
        shift
    ;;
    --keyname|--kn|-kn)
        keyname="$2"
        shift
    ;;
    esac
    shift
done

if [[ $root_certs_generation == "true" ]]; then
    generate_root_certs
fi

if [[ $node_certs_generation == "true" ]]; then
    temp_certs_path="$server_certs_path/temp"
    generate_node_certs
fi

if [[ "$key_generation" == "true" ]]; then
    generate_key
fi
