---
title: Third party software
headerTitle: Third party software
linkTitle: Third party software
description: Learn about the third party software contained in YugabyteDB.
aliases:
 - /stable/
block_indexing: true
menu:
  stable:
    parent: legal
    identifier: third-party-software
    weight:   30
isTocNested: true
showAsideToc: true
---

## Acknowledgements

Yugabyte proudly participates in the open source community and appreciates all open source contributions that have been incorporated into the YugabyteDB open source project. The YugabyteDB codebase has leveraged several open source projects as a starting point, including the following:

- PostgreSQL stateless language layer for implementing YSQL.
- PostgreSQL scanner and parser modules (`.y` and `.l` files) were used as a starting point for implementing the YCQL scanner and parser in C++.
- The DocDB document store uses a customized and enhanced version of [RocksDB](https://github.com/facebook/rocksdb). Some of the customizations and enhancements are described in [DocDB store](https://docs.yugabyte.com/stable/architecture/docdb/).
- The Apache Kudu Raft implementation and server framework were used as a starting point. Since then, Yugabyte has implemented several enhancements, such as leader leases and pre-voting state during learner mode for correctness, improvements to the network stack, auto balancing of tablets on failures, zone/DC aware data placement, leader-balancing, ability to do full cluster moves in a online manner, and more.
- Google libraries (`glog`, `gflags`, `protocol buffers`, `snappy`, `gperftools`, `gtest`, `gmock`).

## Third party software components

Yugabyte products incorporate third party software, which includes the copyrighted, patented, or otherwise legally protected software of third parties. Included here is a list of the third party open source software components that are incorporated into YugabyteDB products.

|  **Component Name** | **Version** | **License** |
| :--- | :--- | :--- |
|  AWS C++ SDK | 1.3.57 | Apache-2.0 |
|  LOOK | 55f9b4c | MIT |
|  Boost | 1.66.0 | BSL-1.0 |
|  Cassandra Query Language Shell | 3.10-yb-3 | Apache-2.0 |
|  Cassandra (DataStax) Python Driver | 3.13.0.post0-743d942c | Apache-2.0 |
|  crcutil | 440ba7babeff77ffad992df3a10c767f184e946e | Apache-2.0 |
|  crypt_blowfish | 79ee670d1a2977328c481d6578387928ff92896a | CC0-1.0 |
|  curl | 7.32.0 | MIT |
|  gflags | 2.1.2 | BSD-3-Clause |
|  glog | 0.3.4 | BSD-3-Clause |
|  gperftools | 2.2.1 | BSD-3-Clause |
|  libbacktrace | ba79a27ee9a62b1be86d0ddae7614c316b7f6fbb | BSD-3-Clause |
|  libev | 4.20 | BSD-2-Clause |
|  libunwind | 1.1a | MIT |
|  lz4 | r130 | BSD-2-Clause |
|  nvml | 0.4-b2 | BSD-3-Clause |
|  Google Protocol Buffers | 3.5.1 | BSD-3-Clause |
|  RapidJSON | 1.1.0 | MIT |
|  Redis (command-line client) | 4.0.1 | BSD-3-Clause |
|  Snappy compression | 1.1.3 | BSD-3-Clause |
|  Squeasel web server | 8ac777a122fccf0358cb8562e900f8e9edd9ed11 | MIT |
|  zlib compression | 1.2.11 | Zlib |
|  RocksDB (significantly modified) | Based on version e8e6cf0173849ee323cf6972121a179d085505b1 | Apache-2.0 |
|  Apache Kudu (significantly modified) | Based on version 1ad16aa0b742a75b86152cd7034f150674070606 | Apache-2.0 |
|  Cassandra (DataStax) Java driver with Yugabyte modifications | 3.9.0-yb-5 | Apache-2.0 |
|  autoconf | 2.69 | GPL-2.0 |
|  automake | 1.15.1 | GPL-2.0 |
|  flex | 2.6.4 | BSD-2-Clause |
|  binutils | 2.29.1 | GPL-2.0 |
|  bison | 3.0.4 | GPL-3.0 |
|  bzip2 | 1.0.6.1 | BSD-4-Clause |
|  gdbm | 1.14 | GPL-3.0 |
|  gettext | 0.19.8.1 | GPL-3.0 |
|  glibc | 2.23 | GPL-2.0 |
|  gmp | 6.1.2.1 | GPL-3.0 |
|  gpatch | 2.7.5 | GPL-3.0 |
|  help2man | 1.47.5 | GPL-3.0 |
|  isl | 0.18 | MIT |
|  libedit | 20191231-3.1 | BSD-3-Clause |
|  libmpc | 1.0.3.1 | LGPL-3.0 |
|  libtool | 2.4.6.2 | GPL-2.0 |
|  libuuid | 1.0.3 | BSD-3-Clause |
|  libxml2 | 2.9.7 | MIT |
|  linux-headers | 4.4.80 | GPL-2.0 |
|  m4 | 1.4.18 | GPL-3.0 |
|  mpfr | 3.1.6 | GPL-3.0 |
|  ncurses | 6.0.4 | MIT |
|  openssl | 1.0.2n | OpenSSL |
|  patchelf | 0.9.1 | GPL-3.0 |
|  pkg-config | 0.29.2_1 | GPL-2.0 |
|  sqlite | 3.21.0 | CC0-1.0 |
|  unzip | 6.0.3 | Info-ZIP |
|  zlib | 1.2.11 | Zlib |
|  PostgreSQL | 11.2 | PostgreSQL |
|  libcds | 2.3.3 | Boost Software License |
|  Flask | 1.0.2 | BSD-3-Clause |
|  Jinja2 | 2.10 | BSD-3-Clause |
|  MarkupSafe | 1.0 | BSD-3-Clause |
|  PyNaCl | 1.2.1 | Apache-2.0 |
|  PyYAML | 3.13 | MIT |
|  Werkzeug | 0.14.1 | BSD-3-Clause |
|  ansible | 2.2.3.0 | GPL-3.0 |
|  ansible-vault | 1.0.4 | GPL-3.0 |
|  apache-libcloud | 2.3.0 | Apache-2.0 |
|  auth0-python | 3.0.0 | MIT |
|  awscli | 1.11.1 | Apache-2.0 |
|  bcrypt | 3.1.4 | Apache-2.0 |
|  boto | 2.49.0 | MIT |
|  boto3 | 1.8.7 | Apache-2.0 |
|  botocore | 1.11.7 | Apache-2.0 |
|  cached-property | 1.5.1 | BSD-3-Clause |
|  cachetools | 2.1.0 | MIT |
|  certifi | 2018.8.24 | MPL-2.0 |
|  cffi | 1.11.5 | MIT |
|  chardet | 3.0.4 | LGPL-2.1 |
|  click | 6.7 | BSD-3-Clause |
|  colorama | 0.3.7 | BSD-3-Clause |
|  cryptography | 1.7.2 | BSD-3-Clause |
|  docker-compose | 1.9.0 | Apache-2.0 |
|  docker-py | 1.7.0 | Apache-2.0 |
|  dockerpty | 0.4.1 | Apache-2.0 |
|  docopt | 0.6.2 | MIT |
|  docutils | 0.14 | CC0-1.0 |
|  enum34 | 1.1.6 | BSD-3-Clause |
|  fabric | 2.2.1 | BSD-2-Clause |
|  functools32 | 3.2.3.post2 | Python-2.0 |
|  futures | 3.2.0 | Python-2.0 |
|  google-api-python-client | 1.7.4 | Apache-2.0 |
|  google-auth | 1.5.1 | Apache-2.0 |
|  google-auth-httplib2 | 0.0.3 | Apache-2.0 |
|  graphviz | 0.9 | MIT |
|  httplib2 | 0.11.3 | MIT |
|  idna | 2.7 | BSD-3-Clause |
|  invoke | 1.1.1 | BSD-2-Clause |
|  ipaddr | 2.1.11 | Apache-2.0 |
|  ipaddress | 1.0.22 | Python-2.0 |
|  itsdangerous | 0.24 | BSD-3-Clause |
|  jmespath | 0.9.0 | MIT |
|  jsonschema | 2.6.0 | MIT |
|  oauth2client | 3.0.0 | Apache-2.0 |
|  paramiko | 2.4.1 | LGPL-2.1 |
|  pyasn1 | 0.4.4 | BSD-2-Clause |
|  pyasn1-modules | 0.2.2 | BSD-2-Clause |
|  pycodestyle | 2.4.0 | MIT |
|  pycparser | 2.18 | BSD-3-Clause |
|  pycrypto | 2.6.1 | CC0-1.0 |
|  python-dateutil | 2.5.3 | Apache-2.0 |
|  python-dotenv | 0.6.4 | BSD-3-Clause |
|  requests | 2.19.1 | Apache-2.0 |
|  rsa | 3.4.2 | Apache-2.0 |
|  s3transfer | 0.1.13 | Apache-2.0 |
|  six | 1.10.0 | MIT |
|  slackclient | 1.2.1 | MIT |
|  stormssh | 0.7.0 | MIT |
|  termcolor | 1.1.0 | MIT |
|  texttable | 1.4.0 | LGPL-3.0 |
|  uritemplate | 3.0.0 | BSD-3-Clause |
|  urllib3 | 1.23 | MIT |
|  web.py | 0.38 | CC0-1.0 |
|  websocket-client | 0.52.0 | LGPL-2.1 |
|  aopalliance | 1.0 | CC0-1.0 |
|  minlog | 1.3.0 | BSD-3-Clause |
|  jnr-constants | 0.9.0 | Apache-2.0 |
|  jnr-x86asm | 1.0.2 | MIT |
|  jsr305 | 3.0.1 | BSD-3-Clause |
|  protobuf-java | 2.6.1 | BSD-3-Clause |
|  protobuf-java | 3.5.1 | BSD-3-Clause |
|  jdbcdslog | 1.0.6.2 | Apache-2.0 |
|  junit-interface | 0.11 | BSD-2-Clause |
|  jsr166e | 1.1.0 | CC0-1.0 |
|  netty-reactive-streams | 1.0.5 | Apache-2.0 |
|  netty-reactive-streams-http | 1.0.5 | Apache-2.0 |
|  spark-cassandra-connector_2.10 | 2.4-yb-2 | Apache-2.0 |
|  commons-beanutils-core | 1.8.0 | Apache-2.0 |
|  metrics-core | 3.1.2 | Apache-2.0 |
|  metrics-graphite | 3.1.2 | Apache-2.0 |
|  metrics-json | 3.1.2 | Apache-2.0 |
|  metrics-jvm | 3.1.2 | Apache-2.0 |
|  jdiff | 1.0.9 | LGPL-2.0 |
|  junit | 4.12 | EPL-1.0 |
|  lz4 | 1.3.0 | Apache-2.0 |
|  pyrolite | 4.13 | MIT |
|  hadoop-client | 2.2.0 | Apache-2.0 |
|  avaje-ebeanorm | 6.18.1 | Apache-2.0 |
|  jetty-io | 9.2.15.v20160210 | Apache-2.0 |
|  websocket-api | 9.2.15.v20160210 | Apache-2.0 |
|  websocket-client | 9.2.15.v20160210 | Apache-2.0 |
|  websocket-common | 9.2.15.v20160210 | Apache-2.0 |
|  aopalliance-repackaged | 2.4.0-b34 | CDDL-1.0 |
|  hibernate-validator | 5.2.4.Final | Apache-2.0 |
|  json4s-jackson_2.10 | 3.2.11 | Apache-2.0 |
|  jbcrypt | 0.3m | ISC |
|  reactive-streams | 1.0.0 | CC0-1.0 |
|  reflections | 0.9.10 | BSD-2-Clause |
|  RoaringBitmap | 0.5.11 | Apache-2.0 |
|  scala-library | 2.11.7 | BSD-3-Clause |
|  scala-library | 2.10.6 | BSD-3-Clause |
|  scalap | 2.10.0 | BSD-3-Clause |
|  scala-parser-combinators_2.11 | 1.0.4 | BSD-3-Clause |
|  scala-xml_2.11 | 1.0.1 | BSD-3-Clause |
|  test-interface | 1.0 | BSD-3-Clause |
|  scala-stm_2.11 | 0.7 | BSD-3-Clause |
|  unused | 1.0.0 | Apache-2.0 |
|  tyrex | 1.0.1 | BSD-4-Clause |
|  xmlenc | 0.52 | BSD-3-Clause |
|  cglib-nodep | 2.1_3 | Apache-2.0 |
|  logback-classic | 1.1.4 | EPL-1.0 |
|  logback-core | 1.1.4 | EPL-1.0 |
|  stream | 2.7.0 | Apache-2.0 |
|  cassandra-driver-core | 3.1.2 | Apache-2.0 |
|  kryo-shaded | 3.0.3 | BSD-3-Clause |
|  classmate | 1.1.0 | Apache-2.0 |
|  jackson-annotations | 2.7.1 | Apache-2.0 |
|  jackson-annotations | 2.6.5 | Apache-2.0 |
|  jackson-core | 2.5.3 | Apache-2.0 |
|  jackson-core | 2.6.5 | Apache-2.0 |
|  jackson-databind | 2.7.1 | Apache-2.0 |
|  jackson-databind | 2.6.5 | Apache-2.0 |
|  jackson-datatype-jdk8 | 2.7.1 | Apache-2.0 |
|  jackson-datatype-jsr310 | 2.7.1 | Apache-2.0 |
|  jackson-module-paranamer | 2.6.5 | Apache-2.0 |
|  jackson-module-scala_2.10 | 2.6.5 | Apache-2.0 |
|  jffi | 1.2.10 | Apache-2.0 |
|  jnr-ffi | 2.0.7 | Apache-2.0 |
|  jnr-posix | 3.0.27 | GPL-2.0 |
|  gson | 2.8.0 | Apache-2.0 |
|  guava | 19.0 | Apache-2.0 |
|  guava | 16.0.1 | Apache-2.0 |
|  guice | 4.0 | Apache-2.0 |
|  guice | 3.0 | Apache-2.0 |
|  guice-assistedinject | 4.0 | Apache-2.0 |
|  h2 | 1.4.191 | EPL-1.0 |
|  bonecp | 0.8.0.RELEASE | Apache-2.0 |
|  compress-lzf | 1.0.3 | Apache-2.0 |
|  murmur | 1.0.0 | Apache-2.0 |
|  async | 1.4.1 | BSD-3-Clause |
|  paranamer | 2.6 | BSD-3-Clause |
|  chill-java | 0.8.0 | Apache-2.0 |
|  chill_2.10 | 0.8.0 | Apache-2.0 |
|  config | 1.3.0 | Apache-2.0 |
|  ssl-config-akka_2.11 | 0.2.1 | Apache-2.0 |
|  ssl-config-core_2.11 | 0.2.1 | Apache-2.0 |
|  akka-actor_2.11 | 2.4.4 | Apache-2.0 |
|  akka-slf4j_2.11 | 2.4.4 | Apache-2.0 |
|  akka-stream_2.11 | 2.4.4 | Apache-2.0 |
|  build-link | 2.5.3 | Apache-2.0 |
|  filters-helpers_2.11 | 2.5.3 | Apache-2.0 |
|  play-cache_2.11 | 2.5.3 | Apache-2.0 |
|  play-datacommons_2.11 | 2.5.3 | Apache-2.0 |
|  play-ebean_2.11 | 3.0.0 | Apache-2.0 |
|  play-enhancer | 1.1.0 | Apache-2.0 |
|  play-exceptions | 2.5.3 | Apache-2.0 |
|  play-functional_2.11 | 2.5.3 | Apache-2.0 |
|  play-iteratees_2.11 | 2.5.3 | Apache-2.0 |
|  play-java-jdbc_2.11 | 2.5.3 | Apache-2.0 |
|  play-java-ws_2.11 | 2.5.3 | Apache-2.0 |
|  play-java_2.11 | 2.5.3 | Apache-2.0 |
|  play-jdbc-api_2.11 | 2.5.0 | Apache-2.0 |
|  play-jdbc-evolutions_2.11 | 2.5.0 | Apache-2.0 |
|  play-jdbc_2.11 | 2.5.3 | Apache-2.0 |
|  play-json_2.11 | 2.5.3 | Apache-2.0 |
|  play-logback_2.11 | 2.5.3 | Apache-2.0 |
|  play-netty-server_2.11 | 2.5.3 | Apache-2.0 |
|  play-netty-utils | 2.5.3 | Apache-2.0 |
|  play-server_2.11 | 2.5.3 | Apache-2.0 |
|  play-streams_2.11 | 2.5.3 | Apache-2.0 |
|  play-test_2.11 | 2.5.3 | Apache-2.0 |
|  play-ws_2.11 | 2.5.3 | Apache-2.0 |
|  play_2.11 | 2.5.3 | Apache-2.0 |
|  twirl-api_2.11 | 1.1.1 | Apache-2.0 |
|  univocity-parsers | 2.2.1 | Apache-2.0 |
|  HikariCP | 2.4.3 | Apache-2.0 |
|  commons-beanutils | 1.9.3 | Apache-2.0 |
|  commons-cli | 1.2 | Apache-2.0 |
|  commons-codec | 1.9 | Apache-2.0 |
|  commons-codec | 1.10 | Apache-2.0 |
|  commons-collections | 3.2.2 | Apache-2.0 |
|  commons-configuration | 1.6 | Apache-2.0 |
|  commons-digester | 1.8 | Apache-2.0 |
|  commons-httpclient | 3.1 | Apache-2.0 |
|  commons-io | 2.4 | Apache-2.0 |
|  commons-lang | 2.5 | Apache-2.0 |
|  commons-logging | 1.2 | Apache-2.0 |
|  commons-net | 2.2 | Apache-2.0 |
|  netty | 3.8.0.Final | Apache-2.0 |
|  netty-all | 4.0.33.Final | Apache-2.0 |
|  netty-buffer | 4.0.36.Final | Apache-2.0 |
|  netty-buffer | 4.0.44.Final | Apache-2.0 |
|  netty-codec | 4.0.37.Final | Apache-2.0 |
|  netty-codec | 4.0.44.Final | Apache-2.0 |
|  netty-codec-http | 4.0.34.Final | Apache-2.0 |
|  netty-common | 4.0.36.Final | Apache-2.0 |
|  netty-common | 4.0.44.Final | Apache-2.0 |
|  netty-handler | 4.0.37.Final | Apache-2.0 |
|  netty-handler | 4.0.44.Final | Apache-2.0 |
|  netty-transport | 4.0.36.Final | Apache-2.0 |
|  netty-transport | 4.0.44.Final | Apache-2.0 |
|  netty-transport-native-epoll | 4.0.36.Final | Apache-2.0 |
|  javax.annotation-api | 1.2 | CDDL-1.0 |
|  javax.el-api | 3.0.0 | CDDL-1.0 |
|  commons-logging | 1 | Apache-2.0 |
|  persistence-api | 1.0 | CDDL-1.0 |
|  javax.servlet-api | 3.1.0 | CDDL-1.0 |
|  jta | 1.1 | CDDL-1.0 |
|  validation-api | 1.1.0.Final | Apache-2.0 |
|  javax.ws.rs-api | 2.0.1 | CDDL-1.1 |
|  jline | 2.12 | BSD-4-Clause |
|  joda-time | 2.9.2 | Apache-2.0 |
|  joda-time | 2.3 | Apache-2.0 |
|  log4j | 1.2.17 | Apache-2.0 |
|  jets3t | 0.7.1 | Apache-2.0 |
|  jna | 4.1.0 | Apache-2.0 |
|  jna-platform | 4.1.0 | Apache-2.0 |
|  typetools | 0.4.4 | Apache-2.0 |
|  ehcache-core | 2.6.11 | Apache-2.0 |
|  py4j | 0.10.4 | BSD-3-Clause |
|  cssparser | 0.9.18 | LGPL-2.0 |
|  htmlunit | 2.20 | Apache-2.0 |
|  htmlunit-core-js | 2.17 | MPL-2.0 |
|  nekohtml | 1.9.22 | Apache-2.0 |
|  signpost-commonshttp4 | 1.2.1.2 | Apache-2.0 |
|  signpost-core | 1.2.1.2 | Apache-2.0 |
|  antlr4-runtime | 4.5.3 | BSD-3-Clause |
|  avro | 1.7.7 | Apache-2.0 |
|  avro-ipc | 1.7.7 | Apache-2.0 |
|  avro-mapred | 1.7.7 | Apache-2.0 |
|  commons-compress | 1.4.1 | Apache-2.0 |
|  commons-crypto | 1.0.0 | Apache-2.0 |
|  commons-exec | 1.3 | Apache-2.0 |
|  commons-lang3 | 3.4 | Apache-2.0 |
|  commons-lang3 | 3.6 | Apache-2.0 |
|  commons-math | 2.1 | Apache-2.0 |
|  commons-math3 | 3.5 | Apache-2.0 |
|  commons-pool2 | 2.4.2 | Apache-2.0 |
|  commons-text | 1.3 | Apache-2.0 |
|  curator-client | 2.4.0 | Apache-2.0 |
|  curator-framework | 2.4.0 | Apache-2.0 |
|  curator-recipes | 2.4.0 | Apache-2.0 |
|  hadoop-annotations | 2.2.0 | Apache-2.0 |
|  hadoop-auth | 2.2.0 | Apache-2.0 |
|  hadoop-common | 2.2.0 | Apache-2.0 |
|  hadoop-hdfs | 2.2.0 | Apache-2.0 |
|  hadoop-mapreduce-client-app | 2.2.0 | Apache-2.0 |
|  hadoop-mapreduce-client-common | 2.2.0 | Apache-2.0 |
|  hadoop-mapreduce-client-core | 2.2.0 | Apache-2.0 |
|  hadoop-mapreduce-client-jobclient | 2.2.0 | Apache-2.0 |
|  hadoop-mapreduce-client-shuffle | 2.2.0 | Apache-2.0 |
|  hadoop-yarn-api | 2.2.0 | Apache-2.0 |
|  hadoop-yarn-client | 2.2.0 | Apache-2.0 |
|  hadoop-yarn-common | 2.2.0 | Apache-2.0 |
|  hadoop-yarn-server-common | 2.2.0 | Apache-2.0 |
|  httpclient | 4.5.2 | Apache-2.0 |
|  httpcore | 4.4.5 | Apache-2.0 |
|  httpmime | 4.5.2 | Apache-2.0 |
|  ivy | 2.4.0 | Apache-2.0 |
|  parquet-column | 1.8.1 | Apache-2.0 |
|  parquet-common | 1.8.1 | Apache-2.0 |
|  parquet-encoding | 1.8.1 | Apache-2.0 |
|  parquet-format | 2.3.0-incubating | Apache-2.0 |
|  parquet-hadoop | 1.8.1 | Apache-2.0 |
|  parquet-jackson | 1.8.1 | Apache-2.0 |
|  spark-catalyst_2.10 | 2.1.0 | Apache-2.0 |
|  spark-core_2.10 | 2.1.0 | Apache-2.0 |
|  spark-launcher_2.10 | 2.1.0 | Apache-2.0 |
|  spark-network-common_2.10 | 2.1.0 | Apache-2.0 |
|  spark-network-shuffle_2.10 | 2.1.0 | Apache-2.0 |
|  spark-sketch_2.10 | 2.1.0 | Apache-2.0 |
|  spark-sql_2.10 | 2.1.0 | Apache-2.0 |
|  spark-tags_2.10 | 2.1.0 | Apache-2.0 |
|  spark-unsafe_2.10 | 2.1.0 | Apache-2.0 |
|  tomcat-servlet-api | 8.0.32 | Apache-2.0 |
|  xbean-asm5-shaded | 4.4 | Apache-2.0 |
|  zookeeper | 3.4.5 | Apache-2.0 |
|  async-http-client | 2.0.2 | Apache-2.0 |
|  netty-codec-dns | 2.0.2 | Apache-2.0 |
|  netty-resolver | 2.0.2 | Apache-2.0 |
|  netty-resolver-dns | 2.0.2 | Apache-2.0 |
|  avaje-ebeanorm-agent | 4.9.1 | Apache-2.0 |
|  jackson-core-asl | 1.9.13 | Apache-2.0 |
|  jackson-mapper-asl | 1.9.13 | Apache-2.0 |
|  commons-compiler | 3.0.0 | BSD-3-Clause |
|  janino | 3.0.0 | BSD-3-Clause |
|  jetty-util | 9.2.15.v20160210 | Apache-2.0 |
|  fluentlenium-core | 0.10.9 | Apache-2.0 |
|  flyway-core | 4.0 | Apache-2.0 |
|  flyway-play_2.11 | 3.0.1 | Apache-2.0 |
|  leveldbjni-all | 1.8 | BSD-3-Clause |
|  hk2-api | 2.4.0-b34 | CDDL-1.0 |
|  hk2-locator | 2.4.0-b34 | CDDL-1.0 |
|  hk2-utils | 2.4.0-b34 | CDDL-1.0 |
|  osgi-resource-locator | 1.0.1 | CDDL-1.0 |
|  javax.inject | 2.4.0-b34 | CDDL-1.0 |
|  jersey-guava | 2.22.2 | CDDL-1.0 |
|  jersey-container-servlet | 2.22.2 | CDDL-1.0 |
