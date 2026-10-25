#!/usr/bin/env python3
"""Sepolia availability boundaries; native tests own cryptographic acceptance."""
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import build_gloas_peer_collector as build
import run_gloas_peer_light_client as peer


def policy():
    return dict(format="b3-ethereum-sepolia-gloas-policy/1", mainnet_enabled=False,
                chain_id=11155111,genesis_validators_root=peer.SEPOLIA_GVR,
                genesis_time=peer.SEPOLIA_GENESIS,seconds_per_slot=12,
                gloas_fork_epoch=peer.GLOAS_EPOCH,gloas_fork_version="0x90000076",
                trusted_root="0x"+"12"*32,trusted_slot=peer.GLOAS_EPOCH*32,
                fork_schedule_valid_through_epoch=peer.REVIEWED_THROUGH_EPOCH,
                max_sync_lag_slots=8192,max_recovery_lag_slots=8192,min_participants=342)


class GloasAdapterTests(unittest.TestCase):
    def test_fixed_network_and_checkpoint_policy(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/"policy.json"
            good=policy();path.write_text(json.dumps(good))
            self.assertEqual(peer.policy_input(path)[1],good)
            for field,value in (("mainnet_enabled",True),("chain_id",1),("chain_id",True),
                    ("genesis_validators_root","0x"+"34"*32),("genesis_time",0),
                    ("gloas_fork_epoch",0),("gloas_fork_version","0x90000075"),
                    ("format","b3-ethereum-peer-policy/1"),("max_recovery_lag_slots",8193),
                    ("max_sync_lag_slots",8193),("min_participants",341),
                    ("fork_schedule_valid_through_epoch",peer.REVIEWED_THROUGH_EPOCH+1),
                    ("trusted_slot",peer.GLOAS_EPOCH*32-1)):
                changed=dict(good,**{field:value});path.write_text(json.dumps(changed))
                with self.subTest(field=field,value=value),self.assertRaises(ValueError):
                    peer.policy_input(path)
            path.write_text(json.dumps(dict(good,verified=True)))
            with self.assertRaises(ValueError):peer.policy_input(path)
            path.write_text(json.dumps(good)[:-1]+',"chain_id":11155111}')
            with self.assertRaises(ValueError):peer.policy_input(path)

    def test_peer_only_arguments_and_cached_bootstrap(self):
        args=peer.backend_arguments(Path("collector"),policy(),"private",19400)
        self.assertIn("--network=sepolia",args);self.assertIn("--no-el",args)
        self.assertNotIn("rpc"," ".join(args).lower())
        self.assertNotIn("--bootstrap-file",args)
        cached=peer.backend_arguments(Path("collector"),policy(),"private",19400,Path("cached"))
        self.assertEqual(cached[-2:],["--bootstrap-file","cached"])
        self.assertEqual(cached[:-2],args)
        for port in (0,True,1023,65534):
            with self.assertRaises(ValueError):peer.backend_arguments(Path("collector"),policy(),"dir",port)

    def test_native_diagnostics_cannot_grant_arbitrum_authority(self):
        good=dict(format="b3-ethereum-sepolia-gloas-verification/1",mainnet_enabled=False,
                  arbitrum_authority=False,writes_enabled=False,chain_id=11155111,
                  genesis_validators_root=peer.SEPOLIA_GVR)
        self.assertEqual(peer.valid_result(good),good)
        for key,value in (("mainnet_enabled",True),("arbitrum_authority",True),
                          ("writes_enabled",True),("chain_id",1),
                          ("genesis_validators_root","0x"+"12"*32)):
            with self.assertRaises(ValueError):peer.valid_result(dict(good,**{key:value}))
        with self.assertRaises(ValueError):peer.valid_result({"verified":True})

    def test_final_clock_fence_wins_over_historical_freshness(self):
        fresh=dict(result="READY",fresh_finalized_execution_hash=True,fenced=False)
        self.assertTrue(peer.final_anchor_verified(fresh))
        for changed in (dict(fresh,fenced=True),dict(fresh,result="BAD_RECORD"),
                        dict(fresh,fresh_finalized_execution_hash=False),None):
            self.assertFalse(peer.final_anchor_verified(changed))

    def test_policy_and_journal_lineage_cannot_change(self):
        row=dict(sequence=2,policy_sha256="11"*32,journal_sha256="22"*32)
        peer.consistent_result(None,row);peer.consistent_result(row,dict(row,sequence=3,journal_sha256="33"*32))
        for changed in (dict(row,sequence=1),dict(row,sequence=True),dict(row,policy_sha256="33"*32),dict(row,journal_sha256="33"*32)):
            with self.assertRaises(ValueError):peer.consistent_result(row,changed)

    def test_private_diagnostic_logs_are_bounded_and_not_authority(self):
        with tempfile.TemporaryDirectory() as directory:
            result={};data=b"private peer diagnostic\n"*(peer.MAX_LOG_BYTES//8)
            peer._drain_log(io.BytesIO(data),result,"backend",Path(directory))
            self.assertTrue(result["backend"]["truncated"])
            self.assertEqual((Path(directory)/"backend.log").stat().st_size,peer.MAX_LOG_BYTES)
            self.assertNotIn("private",json.dumps(result))

    def test_executable_pins_and_clean_child_environment(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/"collector";path.write_bytes(b"one")
            digest=hashlib.sha256(b"one").hexdigest();peer.pinned_binary(path,digest)
            path.write_bytes(b"two")
            with self.assertRaises(ValueError):peer.pinned_binary(path,digest)
        with patch.dict(os.environ,{"PATH":"known","LD_PRELOAD":"inject","ETH_RPC_URL":"secret","PYTHONPATH":"inject"},clear=True):
            self.assertEqual(build.clean_environment(),{"PATH":"known"})


if __name__=="__main__":unittest.main()
