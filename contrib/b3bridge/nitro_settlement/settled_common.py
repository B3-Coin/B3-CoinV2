"""Explicit owned full-rollup adapter; standalone --dev checks stay unchanged."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import time

from runtime import Rpc, load, read_json, status, MODULE_ROOT

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / 'contrib/flowmesh2/nitro_regtest'))
from lab_common import (NitroLabChain, Cast, expect, hex_value, address, LAB_PUBLIC_DEV_KEY,
                        private_new, raw_file, canonical)

_spec = importlib.util.spec_from_file_location('b3_full_settlement_export', HERE / 'export_witness.py')
_export = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(_export)
export_settled = _export.export_settled
SOURCE_MODEL = 'owned-l1-settled-nitro/1'


class SettledLabChain(NitroLabChain):
    def __init__(self, runtime, tools, evidence, source_root=REPO):
        self.runtime, self.provenance = load(Path(runtime))
        expect(self.provenance.get('ready') is True and all(status(runtime)['running'].values()), 'owned full-rollup roles are not ready')
        self.tools, self.evidence, self.source = Path(tools), Path(evidence), Path(source_root)
        self.cast = Cast(self.tools / 'cast'); self.evidence.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.port = self.provenance['ports']['sequencer']; self.url = 'http://127.0.0.1:' + str(self.port)
        self.read = Rpc(self.port); self.l1 = Rpc(self.provenance['ports']['geth'])
        self.validator = Rpc(self.provenance['ports']['validator'])
        self.genesis = hex_value(self.provenance['l2_genesis'], 32)
        self.l1_genesis = hex_value(self.provenance['l1_genesis'], 32)
        self.counter, self.fenced = 0, False
        self.deployment = read_json(self.runtime / 'config/deployment.json')
        self.rollup = self.deployment['rollup'].lower()
        self.event_topic = _export.layout(Path(self.provenance['assets']))['assertion_event_topic']
        self.identity()
        self.dev_address = address(self.cast.run('wallet', 'address', '--private-key', LAB_PUBLIC_DEV_KEY))

    def identity(self):
        result = super().identity()
        expect(int(self.l1.call('eth_chainId'), 16) == 1337 and
               hex_value(self.l1.call('eth_getBlockByNumber', ['0x0', False])['hash'], 32) == self.l1_genesis,
               'owned L1 identity changed')
        return result

    def settled(self):
        self.identity()
        latest = self.l1.call('eth_getStorageAt', [self.rollup, _export.hex32(116), 'latest'])
        base = int.from_bytes(_export.keccak256(_export.raw(latest, 32) + (117).to_bytes(32, 'big')), 'big')
        word = int(self.l1.call('eth_getStorageAt', [self.rollup, _export.hex32(base), 'latest']), 16)
        expect((word >> 200) & 255 == 2, 'owned latest assertion is not confirmed')
        created = (word >> 128) & (2 ** 64 - 1)
        events = self.l1.call('eth_getLogs', [{'address': self.rollup, 'fromBlock': hex(created), 'toBlock': hex(created),
                                            'topics': [self.event_topic, latest]}])
        expect(len(events) == 1 and events[0].get('removed') is False, 'owned assertion preimage unavailable')
        data = _export.raw(events[0]['data']); expect(len(data) == 800, 'owned assertion event ABI changed')
        head_hash = '0x' + data[13 * 32:14 * 32].hex()
        block = self.read.call('eth_getBlockByHash', [head_hash, False])
        expect(block is not None, 'owned settled L2 head unavailable')
        return {'height': int(block['number'], 16), 'hash': head_hash, 'assertion': latest}

    def covered(self, point):
        settled = self.settled()
        validated = self.validator.call('arb_latestValidated')
        if settled['height'] < point['height'] or validated is None or MODULE_ROOT not in validated.get('WasmRoots', []):
            return False
        block = self.read.call('eth_getBlockByHash', [validated['GlobalState']['BlockHash'], False])
        return (block is not None and int(block['number'], 16) >= point['height'] and
                self.read.call('eth_getBlockByNumber', [block['number'], False])['hash'] == block['hash'])

    def point(self):
        point = super().point()
        deadline = time.monotonic() + 180
        while not self.covered(point):
            expect(time.monotonic() < deadline, 'owned assertion/WASM coverage deadline')
            time.sleep(2)
        return point

    def check_point(self, point):
        block = super().check_point(point)
        expect(self.covered(point), 'owned checkpoint lacks actual assertion/WASM coverage')
        return block

    def checkpoint(self, height):
        self.identity()
        block = self.l1.call('eth_getBlockByNumber', [hex(height), False])
        expect(block is not None, 'owned L1 canonical checkpoint unavailable')
        return {'height': height, 'hash': block['hash'].lower(), 'genesis': self.l1_genesis}


def retain_bundle(source, destination):
    """Copy only fixed proof files and SHA-named raw objects, fsync every byte."""
    source, destination = Path(source), Path(destination)
    expect(not source.is_symlink() and source.is_dir(), 'unsafe source bundle directory')
    allowed = {'manifest.json', 'deployment-policy.json', 'ancestor-pages.json', 'l1-header.rlp',
               'l2-settled-header.rlp', 'settlement-proof.bin', 'settlement-group-1.bin', 'history-proof.bin'}
    expect((source / 'objects').is_dir() and not (source / 'objects').is_symlink(), 'raw object directory unavailable')
    files = [p for p in source.iterdir() if p.name in allowed]
    files += list((source / 'objects').iterdir())
    expect(len(files) <= 32776, 'bundle object count exceeds bound')
    destination.mkdir(mode=0o700, parents=True, exist_ok=True)
    (destination / 'objects').mkdir(mode=0o700, exist_ok=True)
    total = 0
    for path in files:
        data = raw_file(path, 32 * 1024 * 1024); total += len(data)
        expect(total <= 128 * 1024 * 1024, 'retained bundle exceeds128MiB')
        if path.parent.name == 'objects':
            expect(len(path.name) == 64 and hashlib.sha256(data).hexdigest() == path.name, 'raw object SHA address changed')
            target = destination / 'objects' / path.name
        else:
            target = destination / path.name
        if target.exists():
            expect(raw_file(target, 32 * 1024 * 1024) == data, 'retained bundle conflict')
        else:
            fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            with os.fdopen(fd, 'wb') as stream:
                stream.write(data); stream.flush(); os.fsync(stream.fileno())
    for path in (destination / 'objects', destination):
        fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)


def policy_binding(policy_path, chain):
    return {'source_model': SOURCE_MODEL, 'l1_genesis': chain.l1_genesis, 'l2_genesis': chain.genesis,
            'baseline_policy_sha256': hashlib.sha256(raw_file(policy_path, 4 * 1024 * 1024)).hexdigest()}
