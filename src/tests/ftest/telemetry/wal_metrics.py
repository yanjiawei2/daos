"""
  (C) Copyright 2018-2024 Intel Corporation.

  SPDX-License-Identifier: BSD-2-Clause-Patent
"""
import time

from ior_utils import write_data
from telemetry_test_base import TestWithTelemetry
from test_utils_pool import add_pool


class WalMetrics(TestWithTelemetry):
    """Tests for new specific metrics to track activity of md_on_ssd.

    :avocado: recursive
    """

    def verify_metrics(self, metrics, **kwargs):
        """Collect and verify telemetry metrics data.

        Args:
            details (str): description of the telemetry data
            metrics (list): list of metric names to collect and verify
            kwargs (dict): optional 'min_value' and 'max_value' arguments for verify_metric_value()
        """
        kwargs['metrics_data'] = self.telemetry.get_metric_values(metrics)
        self.telemetry.display_metric_values(kwargs['metrics_data'])
        return self.telemetry.verify_metric_value(**kwargs)

    def test_wal_commit_metrics(self):
        """JIRA ID: DAOS-11626.

        The WAL commit metrics is per-engine metrics in 'dmabuff', it includes 'wal_sz', 'wal_qd'
        and 'wal_waiters' today (see dma_metrics_init() in src/bio/bio_buffer.c). WAL commit metrics
        are updated on each local transaction (for example, transaction for a update request, etc.)

        Test steps:
        1) Verify WAL commit metrics are 0 before pool creation
        2) Create a pool
        3) Verify WAL commit metrics increase after pool creation

        :avocado: tags=all,daily_regression
        :avocado: tags=hw,medium
        :avocado: tags=telemetry
        :avocado: tags=WalMetrics,test_wal_commit_metrics
        """
        wal_metrics = [item for item in self.telemetry.ENGINE_DMABUFF_METRICS if '_wal_' in item]
        verify_after = {'min_value': 1}
        if not self.server_managers[0].manager.job.using_control_metadata:
            # WAL commit metrics are not expected to increase when not using MD on SSD
            verify_after = {'min_value': 0, 'max_value': 0}

        self.log_step('Creating a pool (dmg pool create)')
        pool = add_pool(self)

        self.log_step('Creating a container (daos container create)')
        container = self.get_container(pool)

        self.log_step('Verify WAL commit metrics before writing data (dmg telemetry metrics query)')
        if not self.verify_metrics(wal_metrics, min_value=0, max_value=0):
            self.fail('Unexpected WAL commit metrics before pool creation')

        self.log_step('Writing data (ior)')
        write_data(self, container)

        self.log_step('Verify WAL commit metrics after writing data (dmg telemetry metrics query)')
        if not self.verify_metrics(wal_metrics, **verify_after):
            self.fail('Unexpected WAL commit metrics after pool creation')

        self.log_step('Test passed')

    def test_wal_reply_metrics(self):
        """JIRA ID: DAOS-11626.

        The WAL replay metrics is per-pool metrics in 'vos_rehydration' under each pool folder, it
        includes 'replay_size', 'replay_time', 'replay_entries', 'replay_count' and
        'replay_transactions' (see vos_metrics_alloc() in src/vos/vos_common.c). WAL replay
        metrics are only updated when open a pool on engine start (or when creating a pool).

        :avocado: tags=all,daily_regression
        :avocado: tags=hw,medium
        :avocado: tags=telemetry
        :avocado: tags=WalMetrics,test_wal_reply_metrics
        """
        wal_metrics = self.telemetry.ENGINE_POOL_VOS_REHYDRATION_METRICS
        verify_after = {'min_value': 1}
        if not self.server_managers[0].manager.job.using_control_metadata:
            # WAL commit metrics are not expected to increase when not using MD on SSD
            verify_after = {'min_value': 0, 'max_value': 0}

        self.log_step('Creating a pool (dmg pool create)')
        pool = add_pool(self)

        self.log_step('Verify WAL reply metrics after pool creation (dmg telemetry metrics query)')
        if not self.verify_metrics(wal_metrics, min_value=0, max_value=0):
            self.fail('Unexpected WAL reply metrics after pool creation')

        self.log_step('Creating a container (daos container create)')
        container = self.get_container(pool)

        self.log_step('Writing data (ior)')
        write_data(self, container)

        self.log_step('Verify WAL reply metrics after writing data (dmg telemetry metrics query)')
        if not self.verify_metrics(wal_metrics, **verify_after):
            self.fail('Unexpected WAL reply metrics after pool creation')

        # self.stop_engines()
        # self.restart_engines()

        self.log_step('Test passed')

    def test_wal_checkpoint_metrics(self):
        """JIRA ID: DAOS-11626.

        The WAL checkpoint metrics is per-pool metrics in 'checkpoint' under each pool folder, it
        includes 'duration', 'dirty_pages', 'dirty_chunks', 'iovs_copied' and 'wal_purged' (see
        vos_chkpt_metrics_init() in src/vos/vos_pool.c). WAL checkpoint metrics are update on
        check pointing, check pointing regularly happens in background (See the 'Checkpoint policy'
        in manual), when there is nothing to be checkpoint-ed (no new commits since last
        checkpoint), the checkpoint would be no-op and metrics won’t updated.

        :avocado: tags=all,daily_regression
        :avocado: tags=hw,medium
        :avocado: tags=telemetry
        :avocado: tags=WalMetrics,test_wal_checkpoint_metrics
        """
        frequency = 5
        wal_metrics = self.telemetry.ENGINE_POOL_CHECKPOINT_METRICS
        verify_after = {'min_value': 1}
        if not self.server_managers[0].manager.job.using_control_metadata:
            # WAL commit metrics are not expected to increase when not using MD on SSD
            verify_after = {'min_value': 0, 'max_value': 0}

        self.log_step('Creating a pool (dmg pool create)')
        pool = add_pool(self, properties=f'checkpoint:timed,checkpoint_freq:{frequency}')
        self.log_step('Creating a container (daos container create)')
        container = self.get_container(pool)
        self.log.info('%s check point frequency: %s seconds', container.pool, frequency)

        self.log_step(
            'Verify WAL checkpoint metrics before pool creation (dmg telemetry metrics query)')
        if not self.verify_metrics(wal_metrics, min_value=0, max_value=0):
            self.fail('Unexpected WAL reply metric values before pool creation')

        self.log_step('Writing data (ior)')
        write_data(self, container)

        self.log_step(f'Waiting for check pointing to complete (sleep {frequency * 2})')
        time.sleep(frequency * 2)

        self.log_step(
            'Verify WAL checkpoint metrics after pool creation (dmg telemetry metrics query)')
        if not self.verify_metrics(wal_metrics, **verify_after):
            self.fail('Unexpected WAL reply metric values after pool creation')

        self.log_step('Test passed')
