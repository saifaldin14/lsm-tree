import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "results" / "rdma"))
import rdma_campaign


class FakeCluster:
    def __init__(self):
        self.config = {
            "client": {"host": "client", "db_slots": [("0-3", 0)]},
            "gpu": {"host": "gpu", "address": "192.0.2.41", "cores": "0-3", "node": 0},
            "cpu_pool": {"host": "cpu", "address": "192.0.2.44", "node": 0},
            "scratch": "/tmp/experiment",
        }
        self.build = "/build/client"
        self.started = []
        self.commands = []
        self.pid = 0

    def run(self, host, command, check=True, timeout=None):
        self.commands.append((host, command))
        if "lsm_system_bench" in command:
            result = {"throughput": {"ops_per_second": 1000}}
            return f"rc db0 0\njson db0 {json.dumps(result)}\n"
        return ""

    def start(self, host, command, log):
        self.pid += 1
        endpoint = f"tcp://{host}:5000"
        self.started.append((host, command, log, self.pid, endpoint))
        return self.pid, endpoint

    def stop(self, host, pid):
        pass

    def statistics(self, host, log):
        return {}

    def load(self, host):
        return {}

    def busy_threads(self, host):
        return 0

    def wait_for_free_threads(self, host, needed):
        return {"met": True}

    def cpu_seconds(self, host, pid):
        return 1.0


class R11TcpCampaignTests(unittest.TestCase):
    def test_ensure_golden_rebuilds_when_done_marker_is_stale(self):
        class StaleGoldenCluster:
            config = {
                "client": {"host": "client"},
                "scratch": "/dev/shm/experiment",
                "golden_scratch": "/tmp/experiment-golden",
            }
            build = "/build/client"

            def __init__(self):
                self.commands = []

            def run(self, host, command, check=True, timeout=None):
                self.commands.append(command)
                if ".done" in command and "CURRENT" not in command:
                    return "done"
                return ""

        cluster = StaleGoldenCluster()
        golden = rdma_campaign.ensure_golden(cluster, 10)

        self.assertEqual(golden, "/tmp/experiment-golden/rdma-golden-10")
        preload = next(command for command in cluster.commands if "--phase preload" in command)
        self.assertIn("test -f /tmp/experiment-golden/rdma-golden-10/CURRENT", preload)
        self.assertIn("touch /tmp/experiment-golden/rdma-golden-10.done", preload)
        self.assertIn("mkdir -p /tmp/experiment-golden", preload)

    def test_database_copy_stops_before_running_without_golden_current(self):
        with tempfile.TemporaryDirectory() as directory:
            scratch = str(Path(directory) / "scratch")
            golden = str(Path(directory) / "missing-golden")
            lines = rdma_campaign.database_copy_lines(scratch, golden, 1)
            script = "\n".join(lines + ["echo RUN_STARTED", 'rm -rf "$D"'])
            result = subprocess.run(["bash", "-c", script], capture_output=True, text=True)

        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("RUN_STARTED", result.stdout)
        self.assertIn("golden database missing CURRENT", result.stderr)

    def test_cpu_pool_uses_selected_host_and_tcp_without_rdma_push(self):
        cluster = FakeCluster()
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(rdma_campaign, "ensure_golden", return_value="/tmp/golden"):
                rdma_campaign.experiment_r11(
                    cluster, Path(directory), repetitions=1, records=10, seconds=(0, 1),
                    n=1, engines=1, pools=["0-1,96-97"], transport="tcp",
                    cpu_pool_host=cluster.config["cpu_pool"],
                )

        cpu_servers = [(host, command) for host, command, *_ in cluster.started
                       if "--backend cpu_merge" in command]
        self.assertEqual(len(cpu_servers), 1)
        self.assertEqual(cpu_servers[0][0], "cpu")
        self.assertIn("--listen tcp://192.0.2.44:0", cpu_servers[0][1])

        database_runs = [command for host, command in cluster.commands if "lsm_system_bench" in command]
        self.assertTrue(any("LSM_REMOTE_ENDPOINT=tcp://cpu:5000" in command for command in database_runs))
        self.assertTrue(all("LSM_RDMA_PUSH=1" not in command for command in database_runs))


if __name__ == "__main__":
    unittest.main()
