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
Unit tests for the YSQL catalog upgrade handling in bin/yugabyted (loaded dynamically; there is
no yugabyted package).

These cover the regression where a patch or minor upgrade within the same PostgreSQL major
version was treated as an incomplete YSQL major catalog upgrade: `yugabyted upgrade ysql_catalog`
exited with an error, and `yugabyted start` kept the tserver pinned to the old release.

Integration coverage lives in scripts/yugabyted/test/yugabyted-test.sh.
"""

import importlib.machinery
import importlib.util
import pathlib
import types
import unittest
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


_YUGABYTED = _load_yugabyted_module()

# Representative `get_ysql_major_version_catalog_state` output from yb-admin, one per state
# (see ClusterAdminClient::GetYsqlMajorCatalogUpgradeState in src/yb/tools/yb-admin_client.cc).
_STATE_DONE = "YSQL major catalog upgrade already completed, or is not required."
_STATE_AWAITING_FINALIZATION = "YSQL major catalog awaiting finalization or rollback."
_STATE_PENDING = "YSQL major catalog upgrade for YSQL major upgrade has not yet started."


class _YugabytedTestCase(unittest.TestCase):
    def _patch(self, target, attribute, **kwargs):
        """Patch target.attribute for the duration of the current test."""
        patcher = mock.patch.object(target, attribute, **kwargs)
        mocked = patcher.start()
        self.addCleanup(patcher.stop)
        return mocked


class TestCheckForYsqlCatalogUpgradeState(_YugabytedTestCase):
    """YBAdminProxy.check_for_ysql_catalog_upgrade_state interprets yb-admin output."""

    def _check(self, out, err="", ret_code=0):
        run_process = self._patch(_YUGABYTED, "run_process", return_value=(out, err, ret_code))
        result = _YUGABYTED.YBAdminProxy.check_for_ysql_catalog_upgrade_state("m:7100")
        run_process.assert_called_once()
        return result

    def test_catalog_already_current_is_treated_as_success(self):
        # Regression test: a patch or minor upgrade leaves the catalog on the current major
        # version. Before the fix this returned 1, which made `upgrade ysql_catalog` fail and
        # kept the tserver pinned to the old release on the next `start`.
        _, ret_code = self._check(_STATE_DONE)
        self.assertEqual(ret_code, 0)

    def test_awaiting_finalization_is_treated_as_success(self):
        # A real major upgrade whose catalog upgrade has run and is awaiting finalization.
        _, ret_code = self._check(_STATE_AWAITING_FINALIZATION)
        self.assertEqual(ret_code, 0)

    def test_pending_keeps_tserver_on_old_release(self):
        # A real major upgrade before the catalog upgrade has run must stay non-zero so that
        # `start` keeps the tserver on the old release.
        _, ret_code = self._check(_STATE_PENDING)
        self.assertEqual(ret_code, 1)

    def test_yb_admin_failure_is_non_zero(self):
        # yb-admin itself failing (for example, masters unreachable) must not be mistaken for a
        # completed catalog upgrade.
        _, ret_code = self._check("", err="Timed out", ret_code=1)
        self.assertEqual(ret_code, 1)


class TestUpgradeYsqlCatalog(_YugabytedTestCase):
    """ControlScript.upgrade_ysql_catalog skips gracefully when no major upgrade is needed."""

    @staticmethod
    def _control_script():
        # upgrade_ysql_catalog only touches self.script and self.configs; a MagicMock with real
        # dicts for the config data is enough to exercise it.
        cs = mock.MagicMock()
        cs.script.is_running.return_value = True
        cs.configs.saved_data = {"current_masters": "m:7100", "read_replica": False}
        cs.configs.temp_data = {}
        return cs

    def test_skips_gracefully_when_catalog_already_current(self):
        # Regression test: `yugabyted upgrade ysql_catalog` for a patch or minor upgrade must
        # not fail. It should detect that no major catalog upgrade is needed, mark it complete,
        # and return without invoking the major catalog upgrade.
        cs = self._control_script()
        self._patch(
            _YUGABYTED.YBAdminProxy, "check_for_ysql_catalog_upgrade_state", return_value=("", 0))
        # start_ysql_catalog_upgrade and the animations are only reached if the skip is absent;
        # patching them keeps that case a clean assert_not_called() failure rather than an error.
        start_upgrade = self._patch(
            _YUGABYTED.YBAdminProxy, "start_ysql_catalog_upgrade", return_value=("", 0))
        self._patch(_YUGABYTED.Output, "init_animation")
        self._patch(_YUGABYTED.Output, "update_animation")
        self._patch(_YUGABYTED.Output, "print_out")

        _YUGABYTED.ControlScript.upgrade_ysql_catalog(cs)

        start_upgrade.assert_not_called()
        self.assertTrue(cs.configs.saved_data["catalog_upgrade_completed"])
        cs.configs.save_configs.assert_called_once()

    def test_runs_catalog_upgrade_when_pending(self):
        # A real major upgrade (catalog state pending) must still invoke the major catalog
        # upgrade rather than skipping it.
        cs = self._control_script()
        self._patch(
            _YUGABYTED.YBAdminProxy, "check_for_ysql_catalog_upgrade_state", return_value=("", 1))
        start_upgrade = self._patch(
            _YUGABYTED.YBAdminProxy, "start_ysql_catalog_upgrade", return_value=("", 0))
        self._patch(_YUGABYTED.Output, "init_animation")
        self._patch(_YUGABYTED.Output, "update_animation")
        self._patch(_YUGABYTED.Output, "print_out")

        _YUGABYTED.ControlScript.upgrade_ysql_catalog(cs)

        start_upgrade.assert_called_once()


if __name__ == "__main__":
    unittest.main()
