# Copyright (c) YugabyteDB, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
# in compliance with the License.  You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software distributed under the License
# is distributed on the "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
# or implied.  See the License for the specific language governing permissions and limitations
# under the License.

"""
Unit tests for master address handling in bin/yugabyted (loaded dynamically; there is no
yugabyted package).

yugabyted only ever adds to current_masters, so an address that stops resolving stays there. Both
an empty entry (--tserver_master_addrs=,host:7100) and a hostname left over from a previous run
(a container hostname, say) reach yb-tserver, whose ResolveMasterAddresses() retries each entry
for master_discovery_timeout_ms (1 hour by default) before failing the startup, logging nothing
past "Initializing tablet server...". yugabyted then persisted the bad value, so every later start
hung the same way.

Integration coverage lives in scripts/yugabyted/test/yugabyted-test.sh.
"""

import importlib.machinery
import importlib.util
import json
import os
import pathlib
import socket
import tempfile
import types
import unittest
from typing import Any, ClassVar, Dict, List
from unittest import mock


def _repo_root() -> pathlib.Path:
    return pathlib.Path(__file__).resolve().parents[2]


def _load_yugabyted_module() -> types.ModuleType:
    yugabyted_path = _repo_root() / "bin" / "yugabyted"
    loader = importlib.machinery.SourceFileLoader("_yugabyted_under_test", str(yugabyted_path))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    assert spec is not None
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


class TestParseMasterAddrsCsv(unittest.TestCase):
    yugabyted: ClassVar[types.ModuleType]

    @classmethod
    def setUpClass(cls) -> None:
        cls.yugabyted = _load_yugabyted_module()

    def test_empty_entries_are_dropped(self) -> None:
        parse = self.yugabyted.parse_master_addrs_csv
        self.assertEqual(parse(",host:7100"), ["host:7100"])
        self.assertEqual(parse("a:7100,,b:7100"), ["a:7100", "b:7100"])
        self.assertEqual(parse(" a:7100 , b:7100 "), ["a:7100", "b:7100"])

    def test_no_addresses_gives_empty_list(self) -> None:
        parse = self.yugabyted.parse_master_addrs_csv
        # "".split(",") returns [""], which is truthy and passes an "if not list" guard.
        self.assertEqual(parse(""), [])
        self.assertEqual(parse(None), [])

    def test_populated_list_is_unchanged(self) -> None:
        parse = self.yugabyted.parse_master_addrs_csv
        self.assertEqual(parse("a:7100,b:7100"), ["a:7100", "b:7100"])


class TestConfigMasterAddrsRecovery(unittest.TestCase):
    """parse_config_file() should drop a host-less entry left behind by an older version."""

    yugabyted: ClassVar[types.ModuleType]

    @classmethod
    def setUpClass(cls) -> None:
        cls.yugabyted = _load_yugabyted_module()

    def _load_saved_masters(self, saved_data: Dict[str, Any]) -> Any:
        with tempfile.TemporaryDirectory() as tmp_dir:
            config_file = os.path.join(tmp_dir, "yugabyted.conf")
            with open(config_file, "w") as f:
                json.dump(saved_data, f)
            configs = self.yugabyted.Configs.parse_config_file(config_file, tmp_dir)
            return configs.saved_data.get("current_masters")

    def test_host_less_entry_is_dropped_on_load(self) -> None:
        self.assertEqual(
            self._load_saved_masters({"current_masters": ",host:7100"}), "host:7100")

    def test_valid_list_is_left_alone(self) -> None:
        self.assertEqual(
            self._load_saved_masters({"current_masters": "a:7100,b:7100"}), "a:7100,b:7100")

    def test_empty_value_stays_empty(self) -> None:
        self.assertEqual(self._load_saved_masters({"current_masters": ""}), "")


class TestSplitMasterAddr(unittest.TestCase):
    yugabyted: ClassVar[types.ModuleType]

    @classmethod
    def setUpClass(cls) -> None:
        cls.yugabyted = _load_yugabyted_module()

    def test_host_and_port(self) -> None:
        self.assertEqual(self.yugabyted.split_master_addr("host:7100"), ("host", "7100"))

    def test_ipv6_is_unwrapped(self) -> None:
        # get_url_from_ip() brackets IPv6 addresses, so a bare rpartition(":") would split
        # inside the address itself.
        self.assertEqual(self.yugabyted.split_master_addr("[::1]:7100"), ("::1", "7100"))
        self.assertEqual(self.yugabyted.split_master_addr("[::1]"), ("::1", None))

    def test_bare_host_has_no_port(self) -> None:
        self.assertEqual(self.yugabyted.split_master_addr("host"), ("host", None))


class TestIsResolvableMasterAddr(unittest.TestCase):
    yugabyted: ClassVar[types.ModuleType]

    @classmethod
    def setUpClass(cls) -> None:
        cls.yugabyted = _load_yugabyted_module()

    def test_resolvable_host(self) -> None:
        with mock.patch.object(self.yugabyted.socket, "getaddrinfo", return_value=[]) as lookup:
            self.assertTrue(self.yugabyted.is_resolvable_master_addr("host:7100"))
        lookup.assert_called_once_with("host", None)

    def test_unknown_host(self) -> None:
        with mock.patch.object(
                self.yugabyted.socket, "getaddrinfo", side_effect=socket.gaierror("no such host")):
            self.assertFalse(self.yugabyted.is_resolvable_master_addr("gone:7100"))

    def test_host_less_address(self) -> None:
        self.assertFalse(self.yugabyted.is_resolvable_master_addr(":7100"))

    def test_resolver_error_keeps_the_address(self) -> None:
        # Anything other than a definite lookup failure is not evidence that the master is gone.
        with mock.patch.object(
                self.yugabyted.socket, "getaddrinfo", side_effect=OSError("resolver is unwell")):
            self.assertTrue(self.yugabyted.is_resolvable_master_addr("host:7100"))


class TestPruneUnresolvableMasterAddrs(unittest.TestCase):
    yugabyted: ClassVar[types.ModuleType]

    @classmethod
    def setUpClass(cls) -> None:
        cls.yugabyted = _load_yugabyted_module()

    def _prune(self, addrs: List[str], resolvable: List[str], keep_addr: Any = None) -> Any:
        with mock.patch.object(
                self.yugabyted, "is_resolvable_master_addr",
                side_effect=lambda addr: addr in resolvable):
            return self.yugabyted.prune_unresolvable_master_addrs(addrs, keep_addr)

    def test_stale_address_is_dropped(self) -> None:
        kept, dropped = self._prune(
            ["2647c43801a7:7100", "127.0.0.1:7100", "yugabyte:7100"],
            resolvable=["127.0.0.1:7100", "yugabyte:7100"])
        self.assertEqual(kept, ["127.0.0.1:7100", "yugabyte:7100"])
        self.assertEqual(dropped, ["2647c43801a7:7100"])

    def test_order_is_preserved(self) -> None:
        addrs = ["c:7100", "a:7100", "b:7100"]
        kept, dropped = self._prune(addrs, resolvable=addrs)
        self.assertEqual(kept, addrs)
        self.assertEqual(dropped, [])

    def test_duplicates_are_collapsed(self) -> None:
        kept, _ = self._prune(["a:7100", "a:7100", "b:7100"], resolvable=["a:7100", "b:7100"])
        self.assertEqual(kept, ["a:7100", "b:7100"])

    def test_keep_addr_survives_a_failed_lookup(self) -> None:
        kept, dropped = self._prune(
            ["gone:7100", "self:7100"], resolvable=[], keep_addr="self:7100")
        self.assertEqual(kept, ["self:7100"])
        self.assertEqual(dropped, ["gone:7100"])

    def test_total_resolution_failure_leaves_the_list_alone(self) -> None:
        # A DNS outage must not empty the list: with no address at all the tserver cannot find
        # the cluster even once DNS recovers.
        addrs = ["a:7100", "b:7100"]
        kept, dropped = self._prune(addrs, resolvable=[])
        self.assertEqual(kept, addrs)
        self.assertEqual(dropped, [])


class TestUpdateTserverMasterAddrs(unittest.TestCase):
    """update_tserver_master_addrs() builds the flag the tserver is started with."""

    yugabyted: ClassVar[types.ModuleType]

    @classmethod
    def setUpClass(cls) -> None:
        cls.yugabyted = _load_yugabyted_module()

    def _run_update(
            self, current_masters: str, stale: Any = ()) -> types.SimpleNamespace:
        script = self.yugabyted.ControlScript.__new__(self.yugabyted.ControlScript)
        tserver_cmd = ["/home/yugabyte/bin/yb-tserver", "--tserver_master_addrs=host:7100"]
        script.processes = {"tserver": types.SimpleNamespace(cmd=tserver_cmd)}
        script.configs = types.SimpleNamespace(saved_data={
            "current_masters": current_masters,
            "master_rpc_port": 7100,
        })
        script.advertise_ip = lambda: "host"
        # Stub the resolver so the result does not depend on the test host's DNS.
        with mock.patch.object(
                self.yugabyted, "is_resolvable_master_addr",
                side_effect=lambda addr: addr not in stale):
            script.update_tserver_master_addrs()
        return types.SimpleNamespace(
            cmd=tserver_cmd, saved_data=script.configs.saved_data)

    def test_empty_current_masters_keeps_existing_flag(self) -> None:
        result = self._run_update("")
        self.assertEqual(
            [arg for arg in result.cmd if arg.startswith("--tserver_master_addrs=")],
            ["--tserver_master_addrs=host:7100"])

    def test_host_less_entry_is_not_passed_to_tserver(self) -> None:
        result = self._run_update(",host:7100")
        flags = [arg for arg in result.cmd if arg.startswith("--tserver_master_addrs=")]
        self.assertEqual(flags, ["--tserver_master_addrs=host:7100"])
        self.assertEqual(result.saved_data["current_masters"], "host:7100")

    def test_stale_address_is_not_passed_to_tserver(self) -> None:
        # "gone" was this node's own hostname on an earlier run; it no longer resolves.
        result = self._run_update("gone:7100,host:7100", stale=("gone:7100",))
        flags = [arg for arg in result.cmd if arg.startswith("--tserver_master_addrs=")]
        self.assertEqual(flags, ["--tserver_master_addrs=host:7100"])
        self.assertEqual(result.saved_data["current_masters"], "host:7100")

    def test_multiple_masters_are_preserved(self) -> None:
        result = self._run_update("a:7100,b:7100")
        flags = [arg for arg in result.cmd if arg.startswith("--tserver_master_addrs=")]
        self.assertEqual(len(flags), 1)
        addrs = flags[0].split("=", 1)[1].split(",")
        self.assertEqual(sorted(addrs), ["a:7100", "b:7100", "host:7100"])


if __name__ == "__main__":
    unittest.main()
