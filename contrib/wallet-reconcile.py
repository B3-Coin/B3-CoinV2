#!/usr/bin/env python3
"""Read-only, bounded wallet/validated-UTXO comparison. Never repairs a wallet.

RPC must already be running. Public descriptors can expose financial history;
reports are created exclusively with mode 0600. No private descriptors or keys
are requested. An explicit CLI/datadir/wallet/network is mandatory.
"""
import argparse
import datetime
from decimal import Decimal
import hashlib
import json
import os
import pathlib
import re
import subprocess
import time

READ_METHODS = frozenset(('getblockchaininfo', 'getwalletinfo', 'listdescriptors',
    'listunspent', 'listlockunspent', 'listtransactions', 'getstakinginfo',
    'getrawmempool', 'scantxoutset', 'gettxout', 'gettransaction'))


class RpcError(Exception):
    def __init__(self, code):
        self.code = code
        super().__init__(f'RPC failure code {code}; remote message intentionally omitted')


def atoms(value):
    if isinstance(value, float):
        raise ValueError('binary floating point is not accepted for amounts')
    value = Decimal(value) * 1_000_000_000
    if not value.is_finite() or value != value.to_integral_value() or value < 0:
        raise ValueError('invalid native amount precision/range')
    return int(value)


def outpoint(row):
    txid, n = row['txid'], row['vout']
    if not isinstance(txid, str) or not re.fullmatch('[0-9a-f]{64}', txid):
        raise ValueError('invalid public txid')
    if type(n) is not int or not 0 <= n <= 0xffffffff:
        raise ValueError('invalid output index')
    return f'{txid}:{n}'


def tip(info):
    return {'height': info['blocks'], 'hash': info['bestblockhash']}


def is_bare_native_script(script):
    """Classify only exact supported bare forms, never a solved owner suffix.

    B3's Solver reports the owner's ordinary type for STAKE/B3A1 carriers.
    That type alone cannot distinguish native coins from policy assets.
    Multisig/other forms stay unclassified without a verified full parser.
    """
    patterns = {
        'pubkey': r'(?:21(?:02|03)[0-9a-fA-F]{64}|4104[0-9a-fA-F]{128})ac',
        'pubkeyhash': r'76a914[0-9a-fA-F]{40}88ac',
        'scripthash': r'a914[0-9a-fA-F]{40}87',
        'witness_v0_keyhash': r'0014[0-9a-fA-F]{40}',
        'witness_v0_scripthash': r'0020[0-9a-fA-F]{64}',
        'witness_v1_taproot': r'5120[0-9a-fA-F]{64}',
    }
    pattern = patterns.get(script.get('type'))
    encoded = script.get('hex')
    return pattern is not None and isinstance(encoded, str) and re.fullmatch(pattern, encoded) is not None


def collect(rpc, *, max_range=1000, max_outputs=2000, history_limit=200):
    """Use the node's existing scan, not a second chainstate or address index.

    Only known STAKE wrapper scripts are added to descriptor coverage. Unknown
    STAKE/asset wrappers are explicitly outside this first tool's completeness.
    """
    before = rpc('getblockchaininfo')
    wallet = rpc('getwalletinfo')
    descs = rpc('listdescriptors', False)['descriptors']
    scan_objects = []
    coverage = []
    for item in descs:
        desc = item['desc']
        # A server must honor private=false; do not persist unexpected secrets.
        if re.search(r'(?i)(?:xprv|tprv|prv\()', desc):
            raise ValueError('unexpected private descriptor')
        spec = {'desc': desc}
        if 'range' in item:
            lo, hi = item['range']
            if lo < 0 or hi < lo or hi-lo+1 > max_range:
                raise ValueError('descriptor range exceeds explicit bound; no truncated successful scan')
            spec['range'] = [lo, hi]
        scan_objects.append(spec)
        coverage.append({'descriptor_sha256': hashlib.sha256(desc.encode()).hexdigest(),
                         'range': spec.get('range'), 'timestamp': item.get('timestamp')})
    listed = rpc('listunspent', 0, 9999999, [], True, {'include_immature_coinbase': True})
    listed_keys = {outpoint(x) for x in listed}
    locks = {outpoint(x) for x in rpc('listlockunspent')}
    history = rpc('listtransactions', '*', history_limit, 0, True)
    try:
        stakes = rpc('getstakinginfo').get('stakes', [])
    except RpcError as e:
        if e.code != -32601:
            raise
        stakes = []
    # Full policy script stays intact. No wrapper stripping or signing occurs.
    stake_scripts = {}
    for row in stakes:
        coin = (rpc('gettxout', row['txid'], row['vout'], False) or
                rpc('gettxout', row['txid'], row['vout'], True))
        if coin:
            stake_scripts[outpoint(row)] = coin['scriptPubKey']['hex']
            scan_objects.append({'desc': 'raw('+coin['scriptPubKey']['hex']+')'})
    mp_before = rpc('getrawmempool', False, True)['mempool_sequence']
    scan = rpc('scantxoutset', 'start', scan_objects)
    wanted = {outpoint(x): x for x in scan['unspents']}
    known = {outpoint(x): x for x in listed}
    stake_map = {outpoint(x): x for x in stakes}
    # Bounded recent receive history helps distinguish spent history from UTXOs.
    for row in history:
        if row.get('category') in ('receive', 'generate', 'immature', 'orphan') and 'vout' in row:
            known.setdefault(outpoint(row), row)
    keys = sorted(set(wanted) | set(known) | locks | set(stake_map))
    if len(keys) > max_outputs:
        raise ValueError('matched outputs exceed explicit bound; no partial success report')
    rows = []
    tx_known = {}
    for key in keys:
        txid, n = key.split(':'); n = int(n)
        if txid not in tx_known:
            try:
                tx = rpc('gettransaction', txid, True)
                tx_known[txid] = {'present': True, 'confirmations': tx.get('confirmations')}
            except RpcError as e:
                if e.code != -5:
                    raise
                tx_known[txid] = {'present': False}
        chain = rpc('gettxout', txid, n, False)
        mempool = rpc('gettxout', txid, n, True)
        match = wanted.get(key)
        entry = {'outpoint': key, 'descriptor_matched': match is not None,
                 'wallet_record': tx_known[txid], 'wallet_listed': key in listed_keys,
                 'wallet_locked': key in locks, 'stake': stake_map.get(key),
                 'stake_script': stake_scripts.get(key),
                 'chain': chain, 'mempool_view': mempool,
                 'scan': match,
                 'wallet_spendable': known.get(key, {}).get('spendable'),
                 'wallet_solvable': known.get(key, {}).get('solvable')}
        rows.append(entry)
    mp_after = rpc('getrawmempool', False, True)['mempool_sequence']
    after = rpc('getblockchaininfo')
    return {'version': 1, 'observed_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'chain_before': tip(before), 'chain_after': tip(after),
            'scan_target': {'height': scan['height'], 'hash': scan['bestblock']},
            'scan_success': scan['success'], 'scanned_txouts': scan['txouts'],
            'pruned': after.get('pruned', False), 'pruneheight': after.get('pruneheight'),
            'wallet_private_keys_enabled': wallet.get('private_keys_enabled'),
            'wallet_scanning': wallet.get('scanning'), 'wallet_birthtime': wallet.get('birthtime'),
            'descriptor_coverage': coverage, 'history_limit': history_limit,
            'history_limit_reached': len(history) >= history_limit,
            'mempool_sequence_before': mp_before, 'mempool_sequence_after': mp_after,
            'records': rows}


def analyze(observation):
    stable = observation['chain_before'] == observation['chain_after'] == observation['scan_target']
    target = observation['scan_target']['hash']
    # Individually queried coins must also refer to the exact scan target.
    stable = stable and all(not coin or coin.get('bestblock') == target
        for r in observation['records'] for coin in (r['chain'], r['mempool_view']))
    stable_mp = observation['mempool_sequence_before'] == observation['mempool_sequence_after']
    reasons = []
    if not stable: reasons.append('chain_changed')
    if not stable_mp: reasons.append('mempool_changed')
    if not observation['scan_success']: reasons.append('scan_incomplete')
    if observation.get('wallet_scanning') not in (None, False): reasons.append('wallet_scan_in_progress')
    if observation.get('history_limit_reached'): reasons.append('recent_history_limit_reached')
    results = []
    seen = set()
    for row in observation['records']:
        if row['outpoint'] in seen:
            raise ValueError('duplicate observation outpoint')
        seen.add(row['outpoint'])
        flags = []
        chain, mp = row['chain'], row['mempool_view']
        if not stable: flags.append('chain_changed_comparison_inconclusive')
        if not stable_mp: flags.append('mempool_changed_non_atomic_observation')
        if not observation['scan_success']: flags.append('scan_incomplete')
        if chain and not mp: flags.append('pending_mempool_spend' if stable_mp else 'possible_mempool_spend')
        if not chain and mp: flags.append('unconfirmed_output')
        if chain and row['descriptor_matched'] and not row['wallet_record']['present']:
            flags.append('chain_script_match_missing_wallet_history')
        if not chain and not mp and row['wallet_record']['present']:
            flags.append('wallet_history_output_not_in_active_utxo_set')
        if row['wallet_locked']: flags.append('user_locked')
        if observation.get('wallet_private_keys_enabled') is False:
            flags.append('no_local_private_keys_watch_only_or_external_signer')
        if row.get('wallet_spendable') is False: flags.append('wallet_reports_not_spendable')
        if row.get('wallet_solvable') and not row.get('wallet_spendable'):
            flags.append('solvable_is_not_spending_authority')
        if chain and chain.get('coinbase'):
            flags.append('reward_maturity_requires_current_chain_rules')
        coin = chain or mp
        if coin and not coin.get('coinbase'):
            # This RPC does not expose fCoinStake. A false coinbase field is
            # not evidence that a historical reward is mature (or ordinary).
            flags.append('legacy_coinstake_maturity_not_exposed_by_gettxout')
        asset = {'asset': 'UNCLASSIFIED', 'native_carrier_atoms': atoms(coin['value'])} if coin else None
        if row.get('stake'):
            flags.append('stake_activation_is_not_spend_maturity')
            script = coin['scriptPubKey']['hex'] if coin else ''
            envelope = re.fullmatch(r'2642335331([0-9a-f]{64})000075((?:[0-9a-f]{2})+)', script)
            # Recognize only a narrow full-script subset; do not duplicate
            # permissive policy decoding or infer support from owner type alone.
            known_owner = envelope and any(is_bare_native_script({'type':kind, 'hex':envelope[2]})
                                          for kind in ('pubkey', 'pubkeyhash'))
            matching_stake = (coin and script == row.get('stake_script') and envelope and
                known_owner and atoms(coin['value']) > 0 and
                envelope[1] != '00'*32 and envelope[1] == row['stake'].get('validator_key') and
                atoms(coin['value']) == atoms(row['stake']['amount']))
            if matching_stake:
                asset = {'asset': 'B3', 'amount_atoms': atoms(coin['value']),
                         'policy': 'STAKE', 'activation_status': row['stake']['status']}
            else:
                flags.append('stake_history_without_matching_current_coin')
                if coin and 'stake_evidence_mismatch' not in reasons:
                    reasons.append('stake_evidence_mismatch')
        elif coin and is_bare_native_script(coin['scriptPubKey']):
            asset = {'asset': 'B3', 'amount_atoms': atoms(coin['value']), 'policy': 'standard_script'}
        elif coin:
            # Never reinterpret B3A1/policy payloads or aggregate colored units
            # from gettxout's native carrier value. Exact script is preserved.
            flags.append('asset_policy_classification_required_before_totalling')
            if 'unsupported_policy' not in reasons: reasons.append('unsupported_policy')
        results.append({'outpoint': row['outpoint'], 'flags': flags,
                        'value': asset, 'scriptPubKey': coin['scriptPubKey']['hex'] if coin else None,
                        'creation_height': row.get('scan', {}).get('height') if row.get('scan') else None,
                        'confirmations': coin.get('confirmations') if coin else None,
                        'spending_authority': 'not_proven_by_scan'})
    return {'target': observation['scan_target'], 'chain_stable': stable,
            'mempool_stable': stable_mp, 'scan_complete': observation['scan_success'],
            'status': 'INCONCLUSIVE' if reasons else 'BOUNDED_COMPARISON',
            'inconclusive_reasons': reasons,
            'records': results, 'full_wallet_recovery_proven': False,
            'limitations': ['No spent-history reconstruction or automatic rescan.',
                'Unknown derivation ranges and unknown STAKE/asset wrappers are not covered.',
                'Only exact supported bare scripts are classified as standard native; multisig/other shapes require separate classification.',
                'Native carrier values are not colored-asset amounts; no aggregate balance claimed.',
                'No private-key, label, script-conversion, maturity or authorization recovery is inferred.',
                'gettxout does not expose legacy coinstake status; coinbase=false does not prove maturity.',
                'RPC observations are sequential; unchanged endpoints do not prove absence of an ABA reorg.',
                'Pruned block availability and scan range must be checked before any separately approved rescan.']}


class CliRpc:
    def __init__(self, cli, datadir, wallet, network, deadline):
        self.args = [str(pathlib.Path(cli).resolve()), '-datadir='+str(pathlib.Path(datadir).resolve()),
                     '-regtest=0', '-signet=0', '-testnet=0', '-testnet4=0',
                     '-chain='+network, '-rpcwallet='+wallet]
        # Use one explicit selector. A config's regtest=1 plus -chain=regtest
        # is rejected even though both name the same chain. These client-only
        # overrides neither edit the config nor reconfigure the running node.
        self.network = network
        self.last_method = None
        self.deadline = deadline

    def __call__(self, method, *params):
        if method not in READ_METHODS: raise ValueError('non-read-only method forbidden')
        self.last_method = method
        if method == 'scantxoutset' and params[0] not in ('start', 'status', 'abort'):
            raise ValueError('unknown scan operation')
        timeout = min(300, self.deadline-time.monotonic())
        if timeout <= 0: raise TimeoutError('comparison deadline reached')
        # Public descriptors still disclose ownership/history. Keep all RPC
        # parameters out of process listings; -stdin uses one argument per line.
        encoded = [p if isinstance(p,str) else json.dumps(p) for p in params]
        if any('\n' in p or '\r' in p for p in encoded):
            raise ValueError('line breaks are not accepted in CLI parameters')
        proc = subprocess.run(self.args+['-stdin', method],
                              input=''.join(p+'\n' for p in encoded),
                              capture_output=True, text=True, timeout=timeout)
        if proc.returncode:
            match = re.search(r'error code:\s*(-?\d+)', proc.stderr)
            raise RpcError(int(match[1]) if match else None)
        # b3coin-cli ParseResult intentionally emits no bytes for JSON null.
        # Only gettxout is nullable in this workflow. Never treat another empty
        # response as a successful zero balance or a complete scan.
        if method == 'gettxout' and not proc.stdout.strip():
            return None
        result = json.loads(proc.stdout, parse_float=Decimal)
        if method == 'getblockchaininfo' and result.get('chain') != self.network:
            raise ValueError('RPC node chain differs from explicitly requested network')
        return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli',required=True); parser.add_argument('--datadir',required=True)
    parser.add_argument('--wallet',required=True); parser.add_argument('--network',choices=['main','test','testnet4','signet','regtest'],required=True)
    parser.add_argument('--output',required=True); parser.add_argument('--max-range',type=int,default=1000)
    parser.add_argument('--deadline-seconds',type=int,default=300)
    args=parser.parse_args()
    if args.deadline_seconds<=0 or args.max_range<=0: parser.error('bounds must be positive')
    # Exclusive protected output is reserved before starting expensive work.
    fd=os.open(args.output,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    rpc=CliRpc(args.cli,args.datadir,args.wallet,args.network,time.monotonic()+args.deadline_seconds)
    with os.fdopen(fd,'w') as report:
        try:
            observation=collect(rpc,max_range=args.max_range)
            result={'observation':observation,'analysis':analyze(observation)}
        except (Exception,KeyboardInterrupt) as error:
            result={'status':'INCOMPLETE','error_type':type(error).__name__,
                    'failed_rpc_method':rpc.last_method,
                    'rpc_error_code':error.code if isinstance(error,RpcError) else None,
                    'warning':'No successful zero balance implied. An interrupted node scan may still be running; inspect scantxoutset status before aborting only the scan you own.'}
            json.dump(result,report,indent=2);report.write('\n');raise SystemExit(1)
        json.dump(result,report,indent=2,default=str);report.write('\n')
    print('Protected report written: '+result['analysis']['status']+
          '; not proof of complete history or recovery. No repair, signing or broadcast performed.')
    if result['analysis']['status'] == 'INCONCLUSIVE':
        raise SystemExit(2)


if __name__=='__main__': main()
