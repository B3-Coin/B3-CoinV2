"""Keep disclosed TEST assumptions equal to actual harness constants."""
import json
from pathlib import Path
import sys
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from fm_process_harness import CALL_SECONDS
from fm_protocol import PROFILE as AGREEMENT_PROFILE
import fm_disk_store as storage
import fm_codec as codec


class StorageProfileTests(unittest.TestCase):
    def test_profile_has_explicit_test_domain_and_baseline(self):
        profile = json.loads((HERE / "TEST_PROFILE.json").read_text())
        self.assertIs(profile["test_only"], True)
        self.assertEqual(profile["profile_id"], "flowmesh-v2-disk-recovery-test/2")
        self.assertEqual(profile["baseline"], "eb73e12d8b042f3ccdfd1e0d96701460e07f20d5")
        self.assertEqual(profile["reviewed_revision"], "1d022dbf0da63636d6d43650d3864222761497a1")
        self.assertEqual(profile["agreement_profile"], AGREEMENT_PROFILE["profile_id"])
        self.assertEqual(profile["schedule_seeds"], [31031, 31032])

    def test_disclosed_storage_versions_and_bounds_match_implementation(self):
        profile = json.loads((HERE / "TEST_PROFILE.json").read_text())
        self.assertEqual(profile["store_schema"], storage.SCHEMA_VERSION)
        self.assertEqual(profile["codec_version"], codec.CODEC_VERSION)
        self.assertEqual(profile["storage_fault_points"], list(storage.STAGES))
        self.assertEqual(profile["limits"], {
            "journal_entries": storage.MAX_JOURNAL_ENTRIES,
            "journal_bytes": storage.MAX_JOURNAL_BYTES,
            "database_bytes": storage.MAX_DATABASE_BYTES,
            "rows": storage.MAX_ROWS,
            "row_bytes": storage.MAX_ROW_BYTES,
            "codec_bytes": codec.MAX_ENCODED_BYTES,
            "codec_depth": codec.MAX_DEPTH,
            "child_call_seconds": CALL_SECONDS})


if __name__ == "__main__":
    unittest.main()
