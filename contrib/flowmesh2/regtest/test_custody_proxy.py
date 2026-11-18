#!/usr/bin/env python3
"""Actual isolated RPC test: unchanged proxy runtime cannot conceal upgrade."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

from custody import (CHAIN_ID, Q_ASSET, TEST_MNEMONIC, CustodyDriver, CustodyError,
                     LocalRpc, atomic_json, expect)
from custody_policy import pin_custody
from local_process import OwnedProcess, free_ports, wait_until


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', required=True)
    parser.add_argument('--evidence-dir', required=True)
    parser.add_argument('--tools', default='/home/josh/B3-tools/foundry-v1.8.5')
    parser.add_argument('--solc', default='/home/josh/B3-tools/solc-0.8.35')
    args = parser.parse_args()
    source, evidence = Path(args.source_root).resolve(strict=True), Path(args.evidence_dir).resolve()
    if evidence == source or source in evidence.parents or (evidence.exists() and any(evidence.iterdir())):
        raise CustodyError('proxy test requires fresh evidence outside source; never erase existing history')
    evidence.mkdir(parents=True, exist_ok=True)
    tools = Path(args.tools).resolve(strict=True)
    command = [str(tools / 'forge'), 'build', '--root', str(source / 'contracts'), '--offline', '--use', args.solc,
        '--out', str(evidence / 'contracts-out'), '--cache-path', str(evidence / 'contracts-cache'),
        'regtest/RebirthRegtestToken.sol', 'regtest/RebirthCustodyProxyFixture.sol', 'custody/RebirthCustodyVault.sol']
    with (evidence / 'forge-build.log').open('w', encoding='utf-8') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=600)
    url = 'http://127.0.0.1:' + str(free_ports(1)[0])
    process = OwnedProcess([str(tools / 'anvil'), '--host', '127.0.0.1', '--port', url.rsplit(':', 1)[1],
        '--chain-id', str(CHAIN_ID), '--mnemonic', TEST_MNEMONIC, '--accounts', '6', '--silent'], evidence / 'anvil.log')
    try:
        rpc = LocalRpc(url)
        wait_until(lambda: process.process.poll() is None and rpc.assert_fixture(), 20, 'owned local proxy fixture not ready')
        config = {'rpc_url': url, 'cast': str(tools / 'cast'), 'q_asset': Q_ASSET, 'custody_asset': Q_ASSET,
            'instance': '0x' + 'ab' * 32, 'account_count': 6, 'deployer_index': 0}
        driver = CustodyDriver(config, evidence / 'driver')
        token_artifact = evidence / 'contracts-out/RebirthRegtestToken.sol/RebirthRegtestToken.json'
        original, _ = driver.deploy(token_artifact)
        replacement, _ = driver.deploy(token_artifact)
        proxy, _ = driver.deploy(evidence / 'contracts-out/RebirthCustodyProxyFixture.sol/RebirthCustodyProxyFixture.json',
            driver.cast.abi_encode('constructor(address)', original))
        driver.token = config['token'] = proxy
        roster = '[' + ','.join(driver.operator_addresses) + ']'
        vault, _ = driver.deploy(evidence / 'contracts-out/RebirthCustodyVault.sol/RebirthCustodyVault.json',
            driver.cast.abi_encode('constructor(address,bytes32,bytes32,address[4],(uint256,uint64,uint64,uint64))',
                proxy, Q_ASSET, driver.instance, roster, '(31337,30,60,3600)'))
        driver.vault = config['vault'] = vault
        try:
            pin_custody(driver, config)
        except CustodyError as error:
            expect('proxy requires' in str(error), 'unconfigured proxy must refuse specifically for absent implementation authority')
        else:
            raise CustodyError('proxy without implementation pin was silently accepted')
        implementation = {'slot': '0x360894a13ba1a3210667c828492db98dca3e2076cc3735a920a3ca505d382bbc',
            'address': original, 'code_hash': driver.cast.keccak(rpc.call('eth_getCode', [original, 'latest']))}
        pin_custody(driver, config, implementation)
        driver.verify_contract_identity()
        code_before = driver.cast.keccak(rpc.call('eth_getCode', [proxy, 'latest']))
        # Admin mutation is an explicit adversarial action on our fresh local
        # fixture. The same proxy runtime remains installed afterwards.
        receipt = driver.transact(proxy, driver.cast.calldata('upgradeTo(address)', replacement))
        code_after = driver.cast.keccak(rpc.call('eth_getCode', [proxy, 'latest']))
        expect(code_before == code_after, 'proxy runtime must remain byte-identical across implementation upgrade')
        try:
            driver.verify_contract_identity()
        except CustodyError as error:
            expect('implementation changed' in str(error), 'exact implementation change must fence independent observer')
        else:
            raise CustodyError('same-code proxy implementation upgrade escaped custody policy')
        report = {'passed': True, 'mainnet_enabled': False, 'chain_id': 31337,
            'unconfigured_proxy_refused': True, 'proxy_code_unchanged': True, 'implementation_upgrade_fenced': True,
            'upgrade_transaction': receipt['transactionHash'], 'policy': config['custody_policy']}
        atomic_json(evidence / 'custody-proxy-report.json', report)
        print(json.dumps({'passed': True, 'report': str(evidence / 'custody-proxy-report.json')}))
        return 0
    finally:
        process.close()


if __name__ == '__main__':
    sys.exit(main())
