#!/usr/bin/env python3
"""A FlowMesh V2 HTTPS API with made-up operator replies, for B3 Hive tests.

It serves the real gateway (contrib/flowmesh2/regtest/https_api.py) with its
real TLS identity, bounds and event stream, in front of a stand-in operator
whose head advances every 0.3 s. The replies have the fields the regtest
operator sends (src/flowmesh2/dev/regtest_operator.cpp); the numbers are
made up. It writes {url, certificate, certificate_sha256, account} to the
fixture file named on the command line and serves until it is stopped or
its time runs out. Loopback only; nothing here is a real market.

    python3 hive/scripts/v2-api-fixture.py DIR [SECONDS]
"""
import json
import struct
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'contrib' / 'flowmesh2' / 'regtest'))
from https_api import HttpsGateway, create_local_tls  # noqa: E402

Q = '51' * 32
TOKENA = 'a1' * 32
TOKENB = 'b1' * 32
ACCOUNT = 'ac' * 32


def header(height):
    """A 366-byte header with only its height field (offset 73) filled in."""
    data = bytearray(366)
    struct.pack_into('<Q', data, 73, height)
    return data.hex()


class Operator:
    def __init__(self):
        self.start = time.monotonic()
        self.lock = threading.Lock()

    def height(self):
        return 1 + int((time.monotonic() - self.start) / 0.3)

    def envelope(self, height):
        return {'header': header(height), 'roots': {'version': 1}, 'certificate': '00' * 8}

    def __call__(self, kind, value):
        height = self.height()
        if kind == 'info':
            return {'mode': 'regtest-pbft-view-change', 'seat': 0, 'seats': 4, 'quorum': 3, 'height': height,
                    'head_hash': f'{height:064x}', 'state_root': f'{height + 1:064x}',
                    'header': header(height), 'certificate': '00' * 8, 'mainnet_enabled': False,
                    'assets': {'Q': Q, 'TOKENA': TOKENA, 'TOKENB': TOKENB},
                    'markets': {'TOKENA_Q': {'id': 'aa' * 32, 'base': TOKENA, 'quote': Q,
                                             'base_atoms_per_lot': 1000, 'quote_atoms_per_tick': 10},
                                'TOKENB_Q': {'id': 'bb' * 32, 'base': TOKENB, 'quote': Q,
                                             'base_atoms_per_lot': 100, 'quote_atoms_per_tick': 1}},
                    'pending_actions': 0, 'profile_name': 'fm2-regtest-fixture'}
        if kind == 'totals':
            assets = [{'asset': asset, 'deposits': 10 ** 12, 'effects': 0, 'pending': 0, 'fees_held': height,
                       'available': 10 ** 12 - 500 - height, 'reserved': 500, 'protocol_available': height,
                       'account_liabilities': 10 ** 12 - height, 'total_liabilities': 10 ** 12}
                      for asset in (Q, TOKENA, TOKENB)]
            return {'assets': assets, 'conservation': True, **self.envelope(height)}
        if kind == 'account':
            out = {'account': value['account'], 'found': value['account'] == ACCOUNT, **self.envelope(height)}
            if out['found']:
                out.update({'balances': [{'asset': Q, 'available': 250000, 'reserved': 1000},
                                         {'asset': TOKENA, 'available': 7, 'reserved': 0}],
                            'leaf': '00', 'proof': {'index': 0, 'size': 1, 'path': []}, 'verified': True,
                            'next_nonce': 3})
            return out
        if kind == 'withdrawal':
            return {'found': False, **self.envelope(height)}
        raise ValueError('the fixture operator takes no actions')


def main():
    directory = Path(sys.argv[1]).resolve()
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 300
    config = create_local_tls(directory / 'public-api')
    gateway = HttpsGateway({**config, 'stream_seconds': 2}, Operator()).start()
    fixture = {**gateway.info(), 'certificate': config['certificate'], 'account': ACCOUNT}
    (directory / 'fixture.json').write_text(json.dumps(fixture, indent=1))
    print('fixture', json.dumps(fixture), flush=True)
    try:
        time.sleep(seconds)
    except KeyboardInterrupt:
        pass
    finally:
        gateway.close()


if __name__ == '__main__':
    main()
