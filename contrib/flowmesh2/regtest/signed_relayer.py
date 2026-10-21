#!/usr/bin/env python3
"""Private, deployment-bound, replay-safe EIP-155 custody payout relayer.

The dedicated gas key remains in this child process and a 0600 identity file.
Every exact signed transaction is fsynced before publication. Uncertain sends
and source reorgs resume that same raw transaction; no nonce/fee replacement is
invented. This release permits only unforked literal-loopback chain31337.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import signal
import sys

from custody import CustodyDriver, CustodyError, RpcError, WITHDRAWAL_ABI, address, hex_value, uint, word_address
from custody_policy import PROFILE, canonical
from local_process import JsonProcess, raw_hex

# Release-level gas exposure bounds. These cover system relayer expenditure;
# users still authorize principal/service fees only in stable Q.
MAX_GAS_PRICE_WEI = 100_000_000_000
MAX_GAS_LIMIT = 1_000_000
MAX_NATIVE_COST_WEI = 100_000_000_000_000_000
MAX_JOURNAL_BYTES = 33_554_432


def rlp(value):
    if isinstance(value, int):
        if value < 0:
            raise CustodyError('negative RLP integer')
        value = value.to_bytes((value.bit_length() + 7) // 8, 'big')
    if isinstance(value, bytes):
        if len(value) == 1 and value[0] < 0x80:
            return value
        if len(value) < 56:
            return bytes([0x80 + len(value)]) + value
        length = len(value).to_bytes((len(value).bit_length() + 7) // 8, 'big')
        return bytes([0xb7 + len(length)]) + length + value
    if isinstance(value, list):
        body = b''.join(rlp(item) for item in value)
        if len(body) < 56:
            return bytes([0xc0 + len(body)]) + body
        length = len(body).to_bytes((len(body).bit_length() + 7) // 8, 'big')
        return bytes([0xf7 + len(length)]) + length + body
    raise CustodyError('unsupported RLP type')


def decode_transaction(raw):
    encoded = bytes.fromhex(hex_value(raw)[2:])
    if len(encoded) > 1_048_576:
        raise CustodyError('signed relayer transaction exceeds wire bound')
    def item(position):
        if position >= len(encoded):
            raise CustodyError('truncated signed transaction RLP')
        prefix = encoded[position]
        if prefix < 0x80:
            return encoded[position:position + 1], position + 1
        list_value = prefix >= 0xc0
        offset = 0xc0 if list_value else 0x80
        short_max = 0xf7 if list_value else 0xb7
        if prefix <= short_max:
            length, start = prefix - offset, position + 1
        else:
            length_size = prefix - short_max
            start = position + 1 + length_size
            length_data = encoded[position + 1:start]
            if len(length_data) != length_size or not length_data or length_data[0] == 0:
                raise CustodyError('noncanonical signed transaction RLP length')
            length = int.from_bytes(length_data, 'big')
            if length < 56:
                raise CustodyError('noncanonical long signed transaction RLP')
        end = start + length
        if end > len(encoded):
            raise CustodyError('truncated signed transaction RLP body')
        if not list_value:
            result = encoded[start:end]
            if len(result) == 1 and result[0] < 0x80:
                raise CustodyError('noncanonical signed transaction RLP byte')
            return result, end
        values = []
        while start < end:
            value, start = item(start)
            values.append(value)
        if start != end:
            raise CustodyError('signed transaction RLP child exceeds parent')
        return values, end
    fields, end = item(0)
    if (end != len(encoded) or not isinstance(fields, list) or len(fields) != 9 or
            any(not isinstance(value, bytes) for value in fields) or rlp(fields) != encoded):
        raise CustodyError('signed transaction must be exactly one canonical legacy EIP-155 envelope')
    for index in (0, 1, 2, 4, 6, 7, 8):
        if fields[index] and fields[index][0] == 0:
            raise CustodyError('signed transaction contains a noncanonical integer')
    return fields


def private_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w', encoding='utf-8', newline='\n') as stream:
        stream.write(canonical(value) + '\n')
        stream.flush()
        os.fsync(stream.fileno())
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


class DurableSignedRelayer:
    def __init__(self, config):
        if set(config) != {'format', 'datadir', 'worker', 'custody_config'} or config['format'] != 'fm2-signed-relayer/1':
            raise CustodyError('unknown signed relayer configuration')
        self.directory = Path(config['datadir']).resolve()
        self.directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        if self.directory.stat().st_mode & 0o077:
            raise CustodyError('signed relayer directory permissions must be owner-only')
        self.lock = (self.directory / 'exclusive.lock').open('a+b')
        try:
            fcntl.flock(self.lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise CustodyError('another relayer process owns this gas identity/journal') from error
        self.driver = CustodyDriver(config['custody_config'], self.directory / 'observation')
        if self.driver.custody_profile != PROFILE:
            raise CustodyError('signed payout relayer requires the pinned custody-v2 release policy')
        self.binding = {'chain_id': 31337, 'genesis_hash': self.driver.config['custody_policy']['genesis_hash'],
            'instance': self.driver.instance, 'vault': self.driver.vault,
            'custody_policy': self.driver.config['custody_policy']}
        self.worker = JsonProcess([str(Path(config['worker']).resolve(strict=True))], self.directory / 'signer.stderr.log')
        key_path, self.journal = self.directory / 'private-gas-identity.json', self.directory / 'transactions.jsonl'
        if not key_path.exists():
            if self.journal.exists() or (self.directory / 'public-gas-identity.json').exists():
                raise CustodyError('relayer private gas identity missing; refusing replacement or nonce reset')
            generated = self.worker.request('evm_keygen')
            private_json(key_path, {'format': 'fm2-private-gas-identity/1', 'seed': raw_hex(generated['seed']), 'binding': self.binding})
            # Even a never-used identity owns an original empty history. Its
            # presence distinguishes healthy empty history from lost signed
            # intents after an ambiguous send, restart or account-nonce reorg.
            fd = os.open(self.journal, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
            directory_fd = os.open(self.directory, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
        elif not self.journal.exists():
            raise CustodyError('signed relayer original journal missing; refusing empty-history recovery')
        if key_path.stat().st_mode & 0o077:
            raise CustodyError('relayer private gas identity permissions are too broad')
        secret = json.loads(key_path.read_text(encoding='utf-8'))
        if set(secret) != {'format', 'seed', 'binding'} or secret['format'] != 'fm2-private-gas-identity/1' or secret['binding'] != self.binding:
            raise CustodyError('relayer identity belongs to another custody deployment/policy')
        self.seed = raw_hex(secret['seed'])
        self.sender = address(self.worker.request('evm_key_info', seed=self.seed)['address'])
        public = {'format': 'fm2-public-gas-identity/1', 'address': self.sender, 'binding': self.binding}
        public_path = self.directory / 'public-gas-identity.json'
        if public_path.exists():
            if json.loads(public_path.read_text(encoding='utf-8')) != public:
                raise CustodyError('signed relayer public gas identity changed')
        else:
            private_json(public_path, public)
        self.tip, self.sequence, self.intents, self.resolved = '00' * 32, 0, {}, {}
        self.fenced = False
        encoded = self.journal.read_bytes()
        if len(encoded) > MAX_JOURNAL_BYTES or (encoded and not encoded.endswith(b'\n')):
            raise CustodyError('signed relayer journal is torn/oversized; refusing repair/truncation')
        for line in encoded.splitlines():
            record = json.loads(line)
            digest = record.pop('hash')
            if (set(record) != {'sequence', 'previous', 'kind', 'value'} or record['sequence'] != self.sequence + 1 or
                    record['previous'] != self.tip or hashlib.sha256(canonical(record).encode()).hexdigest() != digest):
                raise CustodyError('signed relayer journal sequence/hash chain is corrupt')
            self._index(record['kind'], record['value'])
            self.tip, self.sequence = digest, record['sequence']

    def close(self):
        if hasattr(self, 'worker'):
            self.worker.close()
        if hasattr(self, 'lock'):
            self.lock.close()

    def _index(self, kind, value):
        nonce = uint(value['nonce'], 64)
        if kind == 'signed':
            if (nonce in self.intents or value['binding'] != self.binding or value['payload']['from'] != self.sender or
                    int(value['payload']['nonce'], 16) != nonce or int(value['payload']['chainId'], 16) != 31337 or
                    value['payload']['to'] != self.driver.vault or value['payload']['value'] != '0x0' or
                    self.driver.cast.keccak(hex_value(value['raw_transaction'])) != hex_value(value['transaction_hash'], 32)):
                raise CustodyError('signed relayer intent is conflicting or belongs to another economic authority')
            if nonce != (max(self.intents) + 1 if self.intents else 0):
                raise CustodyError('signed relayer nonces must form an exact contiguous private-identity history')
            self._verify_envelope(value)
            self.intents[nonce] = value
        elif kind == 'receipt':
            if nonce not in self.intents or value['transaction_hash'] != self.intents[nonce]['transaction_hash']:
                raise CustodyError('relayer receipt does not identify an earlier exact signed transaction')
            self.resolved[nonce] = value
        else:
            raise CustodyError('unknown signed relayer durable record')

    def _verify_envelope(self, intent):
        fields, payload = decode_transaction(intent['raw_transaction']), intent['payload']
        actual = [int.from_bytes(fields[0], 'big'), int.from_bytes(fields[1], 'big'),
            int.from_bytes(fields[2], 'big'), '0x' + fields[3].hex(), int.from_bytes(fields[4], 'big'), '0x' + fields[5].hex()]
        expected = [int(payload['nonce'], 16), int(payload['gasPrice'], 16), int(payload['gas'], 16),
            payload['to'], int(payload['value'], 16), payload['data']]
        v = int.from_bytes(fields[6], 'big')
        if actual != expected or len(fields[3]) != 20 or v not in (31337 * 2 + 35, 31337 * 2 + 36):
            raise CustodyError('signed transaction envelope differs from pinned nonce/target/value/calldata/chain')
        digest = self.driver.cast.keccak('0x' + rlp(fields[:6] + [31337, 0, 0]).hex())
        signature = '0x' + fields[7].rjust(32, b'\0').hex() + fields[8].rjust(32, b'\0').hex() + bytes([27 + v - (31337 * 2 + 35)]).hex()
        if len(signature) != 132:
            raise CustodyError('signed relayer signature scalar has invalid length')
        verified = self.driver.cast.run('wallet', 'verify', '--address', self.sender, '--no-hash', digest, signature)
        if (not verified.strip().startswith('Validation succeeded.') or self.sender.lower() not in verified.lower()):
            raise CustodyError('durable raw transaction was not verified under this private gas identity')

    def _append(self, kind, value):
        record = {'sequence': self.sequence + 1, 'previous': self.tip, 'kind': kind, 'value': value}
        digest = hashlib.sha256(canonical(record).encode()).hexdigest()
        line = canonical({**record, 'hash': digest}).encode() + b'\n'
        if len(line) > 4_194_304:
            raise CustodyError('signed relayer record exceeds durable bound')
        current_bytes = self.journal.stat().st_size if self.journal.exists() else 0
        if current_bytes > MAX_JOURNAL_BYTES - len(line):
            self.fenced = True
            raise CustodyError('signed relayer journal capacity exhausted; history retained, explicit archive migration required')
        # Index validation runs before publication. No chain/network side
        # effect happens before this append has completed its fsync barrier.
        try:
            self._index(kind, value)
            with self.journal.open('ab') as stream:
                stream.write(line)
                stream.flush()
                os.fsync(stream.fileno())
            directory = os.open(self.directory, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
        except Exception:
            self.fenced = True
            raise
        self.tip, self.sequence = digest, record['sequence']

    def capacity(self):
        size = self.journal.stat().st_size if self.journal.exists() else 0
        return {'journal_bytes': size, 'journal_capacity_bytes': MAX_JOURNAL_BYTES,
                'journal_remaining_bytes': max(0, MAX_JOURNAL_BYTES - size)}

    def _fork_compliance(self, action, data):
        if 'regtest_arbitrum_fork' not in self.driver.config:
            return
        # The durable action and EXACT already-bound calldata identify the
        # recipient. Never accept a replacement recipient from a resume caller.
        encoded = bytes.fromhex(hex_value(data)[2:])
        identity = hex_value(action['withdrawal_id'], 32)
        if action['operation'] == 'request':
            selector = self.driver.cast.keccak('requestWithdrawal(' + WITHDRAWAL_ABI + ',bytes[])')[:10]
            if len(encoded) < 4 + 9 * 32 or data[:10] != selector or '0x' + encoded[4:36].hex() != identity:
                raise CustodyError('local fork request recovery action differs from its exact signed calldata')
            recipient = word_address(encoded[4 + 4 * 32:4 + 5 * 32])
        elif action['operation'] == 'finalize':
            if data != self.driver.cast.calldata('finalizeWithdrawal(bytes32)', identity):
                raise CustodyError('local fork finalization recovery action differs from its exact signed calldata')
            recipient = self.driver.withdrawal_status(identity)['recipient']
        else:
            raise CustodyError('local fork relayer cannot authorize an unknown custody action')
        from arbitrum_fork import verify_fork_driver
        verify_fork_driver(self.driver, accounts=[recipient], check_compliance=True)

    def _resume(self, nonce, require_success=True):
        if self.fenced:
            raise CustodyError('signed relayer persistence failed; process is fenced until verified recovery')
        if nonce not in self.intents:
            raise CustodyError('unknown signed relayer nonce')
        if not isinstance(require_success, bool):
            raise CustodyError('relayer receipt policy must be an explicit boolean')
        self.driver.verify_contract_identity()
        intent = self.intents[nonce]
        tx_hash, payload = intent['transaction_hash'], intent['payload']
        found = self.driver.rpc.call('eth_getTransactionByHash', [tx_hash])
        if found is not None and not self.driver._same_transaction(found, payload):
            raise CustodyError('relayer transaction hash resolves to another payload')
        receipt = self.driver.rpc.call('eth_getTransactionReceipt', [tx_hash])
        canonical = False
        if receipt is not None:
            block = self.driver.rpc.call('eth_getBlockByNumber', [receipt['blockNumber'], False])
            canonical = block is not None and hex_value(block['hash'], 32) == hex_value(receipt['blockHash'], 32)
        if not canonical:
            same_nonce = self.driver._find_nonce(payload)
            consumed = int(self.driver.rpc.call('eth_getTransactionCount', [self.sender, 'latest']), 16)
            if same_nonce is not None and same_nonce != tx_hash:
                raise CustodyError('same signed nonce resolves to a different transaction hash')
            if same_nonce is None and consumed > nonce:
                raise CustodyError('relayer nonce consumed without its exact saved transaction; refusing replacement')
            if same_nonce is None:
                # This can be the first publication, a dropped send or a reorg.
                # The exact durable raw bytes (and fee) are always reused.
                self._fork_compliance(intent['action'], payload['data'])
                try:
                    reported = hex_value(self.driver.rpc.call('eth_sendRawTransaction', [intent['raw_transaction']]), 32)
                    if reported != tx_hash:
                        raise CustodyError('source returned a different hash for the exact signed transaction')
                except RpcError:
                    if self.driver.rpc.call('eth_getTransactionByHash', [tx_hash]) is None:
                        raise CustodyError('signed send remains ambiguous; resume the exact saved nonce/raw transaction')
        receipt = self.driver.canonical_receipt(tx_hash, require_success=False)
        found = self.driver.rpc.call('eth_getTransactionByHash', [tx_hash])
        if not isinstance(found, dict) or not self.driver._same_transaction(found, payload):
            raise CustodyError('mined relayer transaction differs from its exact durable signed intent')
        value = {'nonce': nonce, 'transaction_hash': tx_hash, 'receipt': receipt}
        if self.resolved.get(nonce) != value:
            self._append('receipt', value)
        if require_success and int(receipt['status'], 16) != 1:
            raise CustodyError('signed custody transaction reverted: ' + tx_hash)
        return receipt

    def pending(self):
        """Describe the earliest exact intent needing recovery; never allocate a nonce."""
        self.driver.verify_contract_identity()
        if self.resolved:
            latest = self.resolved[max(self.resolved)]['receipt']
            canonical = self.driver.rpc.call('eth_getBlockByNumber', [latest['blockNumber'], False])
            if canonical is None or hex_value(canonical['hash'], 32) != hex_value(latest['blockHash'], 32):
                for nonce, saved in sorted(self.resolved.items()):
                    receipt = saved['receipt']
                    block = self.driver.rpc.call('eth_getBlockByNumber', [receipt['blockNumber'], False])
                    if block is None or hex_value(block['hash'], 32) != hex_value(receipt['blockHash'], 32):
                        return {'required': True, 'nonce': nonce, 'action': self.intents[nonce]['action']}
        for nonce, intent in sorted(self.intents.items()):
            if nonce not in self.resolved:
                return {'required': True, 'nonce': nonce, 'action': intent['action']}
        return {'required': False}

    def transact(self, operation, fields):
        if self.fenced:
            raise CustodyError('signed relayer persistence failed; process is fenced until verified recovery')
        self.driver.verify_contract_identity()
        if operation == 'request':
            if set(fields) - {'withdrawal', 'signatures', 'gas', 'require_success'}:
                raise CustodyError('unexpected signed withdrawal relay fields')
            withdrawal, signatures = fields['withdrawal'], fields['signatures']
            if not 3 <= len(signatures) <= 4:
                raise CustodyError('signed relay requires independently supplied custody quorum')
            data = self.driver.cast.calldata('requestWithdrawal(' + WITHDRAWAL_ABI + ',bytes[])',
                self.driver.withdrawal_tuple(withdrawal), '[' + ','.join(hex_value(item, 65) for item in signatures) + ']')
            action = {'operation': operation, 'withdrawal_id': hex_value(withdrawal['id'], 32)}
        elif operation == 'finalize':
            if set(fields) - {'withdrawal_id', 'gas', 'require_success'}:
                raise CustodyError('unexpected signed finalization relay fields')
            action = {'operation': operation, 'withdrawal_id': hex_value(fields['withdrawal_id'], 32)}
            data = self.driver.cast.calldata('finalizeWithdrawal(bytes32)', action['withdrawal_id'])
        else:
            raise CustodyError('relayer cannot sign arbitrary targets, native transfers or calldata')
        self._fork_compliance(action, data)
        require_success = fields.get('require_success', True)
        if not isinstance(require_success, bool):
            raise CustodyError('relayer receipt policy must be an explicit boolean')
        # A stored receipt is evidence of an earlier resolved send, not a
        # promise that it survived the current source branch. Recheck the most
        # recent resolved checkpoint before allocating any further gas nonce.
        # Its unchanged canonical hash anchors the previously verified prefix.
        if self.resolved:
            latest = self.resolved[max(self.resolved)]['receipt']
            block = self.driver.rpc.call('eth_getBlockByNumber', [latest['blockNumber'], False])
            if block is None or hex_value(block['hash'], 32) != hex_value(latest['blockHash'], 32):
                for nonce, saved in sorted(self.resolved.items()):
                    previous = saved['receipt']
                    canonical = self.driver.rpc.call('eth_getBlockByNumber', [previous['blockNumber'], False])
                    if canonical is None or hex_value(canonical['hash'], 32) != hex_value(previous['blockHash'], 32):
                        intent = self.intents[nonce]
                        if intent['action'] == action and intent['payload']['data'] == data:
                            return self._resume(nonce, require_success)
                        raise CustodyError('source removed an earlier signed relayer receipt; resume its exact nonce before new work')
        for nonce, intent in sorted(self.intents.items()):
            if nonce not in self.resolved:
                if intent['action'] != action or intent['payload']['data'] != data:
                    raise CustodyError('resolve the exact earlier signed transaction before assigning a new nonce')
                return self._resume(nonce, require_success)
        nonce = int(self.driver.rpc.call('eth_getTransactionCount', [self.sender, 'pending']), 16)
        if self.intents and nonce <= max(self.intents):
            # A reorg can remove a resolved receipt and roll back the account
            # nonce. Reconcile that saved raw transaction, not new work.
            prior = self.intents.get(nonce)
            if prior is None or prior['action'] != action or prior['payload']['data'] != data:
                raise CustodyError('source nonce regressed; resume the exact earlier signed intent')
            return self._resume(nonce, require_success)
        if self.intents and nonce != max(self.intents) + 1:
            raise CustodyError('private gas nonce advanced outside its durable relayer history')
        payload = {'from': self.sender, 'to': self.driver.vault, 'nonce': hex(nonce), 'chainId': hex(31337),
            'gasPrice': self.driver.rpc.call('eth_gasPrice'), 'value': '0x0', 'data': data, 'type': '0x0'}
        gas = uint(fields['gas']) if fields.get('gas') is not None else int(self.driver.rpc.call('eth_estimateGas', [payload]), 16) * 6 // 5 + 1000
        gas_price = int(payload['gasPrice'], 16)
        if (not 21000 <= gas <= MAX_GAS_LIMIT or not 0 < gas_price <= MAX_GAS_PRICE_WEI or
                gas * gas_price > MAX_NATIVE_COST_WEI):
            raise CustodyError('signed relayer gas limit is outside the explicit release bound')
        payload['gas'] = hex(gas)
        body = [nonce, int(payload['gasPrice'], 16), gas, bytes.fromhex(self.driver.vault[2:]), 0, bytes.fromhex(data[2:])]
        digest = self.driver.cast.keccak('0x' + rlp(body + [31337, 0, 0]).hex())
        self._fork_compliance(action, data)
        signature = self.worker.request('evm_sign', seed=self.seed, digest=raw_hex(digest))
        if address(signature['address']) != self.sender:
            raise CustodyError('gas signer returned another identity')
        raw_signature = bytes.fromhex(raw_hex(signature['signature'], 65))
        recovery = raw_signature[64]
        if recovery not in (27, 28):
            raise CustodyError('gas signer returned invalid EIP-155 recovery parity')
        raw_transaction = '0x' + rlp(body + [31337 * 2 + 35 + recovery - 27,
            int.from_bytes(raw_signature[:32], 'big'), int.from_bytes(raw_signature[32:64], 'big')]).hex()
        intent = {'nonce': nonce, 'action': action, 'binding': self.binding, 'payload': payload,
            'raw_transaction': raw_transaction, 'transaction_hash': self.driver.cast.keccak(raw_transaction)}
        self._append('signed', intent)
        return self._resume(nonce, require_success)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True)
    args = parser.parse_args()
    relayer = None
    try:
        def stop(signum, frame):
            raise SystemExit(128 + signum)
        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGINT, stop)
        relayer = DurableSignedRelayer(json.loads(Path(args.config).read_text(encoding='utf-8')))
        while True:
            line = sys.stdin.buffer.readline(1048577)
            if not line:
                break
            identity = None
            try:
                if len(line) > 1048576 or not line.endswith(b'\n'):
                    raise CustodyError('signed relayer input exceeds bounded framing')
                request = json.loads(line)
                identity, operation = request.pop('id'), request.pop('cmd')
                if operation == 'info' and not request:
                    result = {'address': relayer.sender, 'chain_id': 31337, 'mainnet_enabled': False,
                        'signed_intents': len(relayer.intents), 'resolved_intents': len(relayer.resolved),
                        **relayer.capacity()}
                elif operation == 'resume' and set(request) <= {'nonce', 'require_success'}:
                    result = relayer._resume(uint(request['nonce'], 64), request.get('require_success', True))
                elif operation == 'pending' and not request:
                    result = relayer.pending()
                else:
                    result = relayer.transact(operation, request)
                reply = {'id': identity, 'ok': True, 'result': result}
            except Exception as error:
                reply = {'id': identity, 'ok': False, 'error': {'code': 'SIGNED_RELAYER_REFUSAL', 'detail': str(error), 'fatal': False}}
            print(canonical(reply), flush=True)
    finally:
        if relayer is not None:
            relayer.close()


if __name__ == '__main__':
    main()
