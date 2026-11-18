#!/usr/bin/env python3
"""Optional real HTTPS reads; never signs, broadcasts, forks or enables mainnet.

Configure two independent provider operators explicitly. URLs are read from
environment variables, never command arguments or the public evidence report.
The exact endpoint commitments and genesis pin must already be in the policy.
Missing credentials fail qualification; offline tests are not a live pass.
"""
import argparse
import json
import os
from pathlib import Path
import re
import sys

from arbitrum_rpc import HttpsReadOnlyRpc, strict_json
from custody import CustodyError, atomic_json
from source_finality import SourceFinality, qualify_read_only


def providers_from_environment(config):
    if (not isinstance(config, dict) or set(config) != {'format', 'policy', 'providers'} or
            config['format'] != 'fm2-arbitrum-read-only-config/1'):
        raise CustodyError('unknown read-only provider configuration')
    policy = SourceFinality.validate_policy(config['policy'])
    if policy['format'] != 'fm2-source-finality-policy/2':
        raise CustodyError('HTTPS qualification requires source finality policy/2')
    if not isinstance(config['providers'], dict) or set(config['providers']) != set(policy['provider_ids']):
        raise CustodyError('read-only provider configuration differs from policy')
    providers = {}
    for label in policy['provider_ids']:
        item = config['providers'][label]
        if (not isinstance(item, dict) or set(item) != {'url_env'} or
                not isinstance(item['url_env'], str) or
                not re.fullmatch(r'[A-Z][A-Z0-9_]{0,127}', item['url_env'])):
            raise CustodyError('provider URL must come from one explicitly named environment variable')
        url = os.environ.get(item['url_env'])
        if not url:
            raise CustodyError('missing read-only provider URL/credentials for ' + label)
        binding = policy['provider_bindings'][label]
        rpc = HttpsReadOnlyRpc(url, label, binding['operator_id'], binding['endpoint_id'],
                              chain_id=policy['chain_id'], genesis_hash=policy['genesis_hash'])
        if rpc.binding != binding:
            raise CustodyError('provider endpoint differs from the preapproved read-only policy')
        providers[label] = rpc
    return policy, providers


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True)
    parser.add_argument('--checkpoint', required=True, help='durable high-water/fence file; retain between runs')
    parser.add_argument('--report', required=True, help='public read-only evidence JSON, contains no provider URL')
    args = parser.parse_args(argv)
    try:
        path = Path(args.config).absolute()
        SourceFinality._safe_path(path)
        with path.open('rb') as stream:
            raw = stream.read(SourceFinality.MAX_STATE_BYTES + 1)
        if len(raw) > SourceFinality.MAX_STATE_BYTES:
            raise CustodyError('read-only provider configuration exceeds its bound')
        policy, providers = providers_from_environment(strict_json(raw.decode('utf-8')))
        # Never permit the report to overwrite policy or rollback-fence state.
        report = Path(args.report).absolute()
        checkpoint = Path(args.checkpoint).absolute()
        reserved = (path, report, checkpoint, checkpoint.with_name(checkpoint.name + '.lock'),
                    checkpoint.with_name(checkpoint.name + '.tmp'), report.with_name(report.name + '.tmp'))
        if len({os.path.normcase(str(item)) for item in reserved}) != len(reserved):
            raise CustodyError('configuration, checkpoint and report must have separate paths')
        result = qualify_read_only(policy, providers, checkpoint)
        SourceFinality._safe_path(report)
        SourceFinality._safe_path(report.with_name(report.name + '.tmp'))
        atomic_json(report, result)
        print(json.dumps({'status': 'read-only-qualified', 'checkpoint': result['checkpoint'],
                          'mainnet_enabled': False, 'writes_enabled': False}, sort_keys=True))
        return 0
    except (CustodyError, OSError, ValueError, UnicodeError):
        # Exception details can originate in private config; keep this public.
        print('read-only Arbitrum qualification refused; inspect private configuration/provider availability', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
