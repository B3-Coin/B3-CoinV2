"""Bounded read-only access to an owned Nitro development node.

The returned bytes are availability data.  A native verifier must authenticate
their header, transaction, receipt and state roots before using source facts.
"""

import http.client
import ipaddress
import json
import re


READ_METHODS = frozenset({
    'web3_clientVersion', 'eth_chainId', 'eth_blockNumber',
    'eth_getBlockByNumber', 'eth_getBlockByHash', 'eth_getCode',
    'eth_getProof', 'eth_getBalance', 'eth_getTransactionReceipt',
    'eth_getRawTransactionByBlockHashAndIndex', 'debug_getRawHeader',
    'debug_getRawBlock', 'debug_getRawReceipts',
})
MAX_REPLY = 32 * 1024 * 1024


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate JSON member')
        result[key] = value
    return result


def raw_hex(value, *, maximum=MAX_REPLY, size=None):
    if not isinstance(value, str) or not re.fullmatch(r'0x(?:[0-9a-fA-F]{2})*', value):
        raise ValueError('invalid canonical byte string')
    if len(value) > 2 + 2 * maximum:
        raise ValueError('byte string exceeds bound')
    result = bytes.fromhex(value[2:])
    if size is not None and len(result) != size:
        raise ValueError('wrong byte string length')
    return result


class LocalNitroRpc:
    def __init__(self, port, *, host='127.0.0.1', timeout=10):
        if str(ipaddress.ip_address(host)) != '127.0.0.1':
            raise ValueError('owned Nitro transport requires literal IPv4 loopback')
        if type(port) is not int or not 1024 <= port <= 65535:
            raise ValueError('invalid loopback port')
        if not 0 < timeout <= 30:
            raise ValueError('invalid RPC timeout')
        self.port, self.timeout, self.sequence = port, timeout, 0

    def call(self, method, params=()):
        if method not in READ_METHODS:
            raise ValueError('method outside read-only Nitro availability boundary')
        self.sequence += 1
        data = json.dumps({'jsonrpc': '2.0', 'id': self.sequence,
                           'method': method, 'params': list(params)},
                          separators=(',', ':')).encode()
        if len(data) > 65536:
            raise ValueError('RPC request exceeds bound')
        conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=self.timeout)
        try:
            conn.request('POST', '/', body=data, headers={
                'Content-Type': 'application/json', 'Connection': 'close'})
            response = conn.getresponse()
            if response.status != 200:
                raise RuntimeError('owned Nitro RPC HTTP refusal')
            length = response.getheader('Content-Length')
            if length is not None and (not length.isdecimal() or int(length) > MAX_REPLY):
                raise ValueError('RPC response length exceeds bound')
            payload = response.read(MAX_REPLY + 1)
            if len(payload) > MAX_REPLY:
                raise ValueError('RPC response exceeds bound')
            value = json.loads(payload, object_pairs_hook=unique_object)
            if (not isinstance(value, dict) or value.get('jsonrpc') != '2.0'
                    or type(value.get('id')) is not int or value.get('id') != self.sequence or 'error' in value
                    or set(value) != {'jsonrpc', 'id', 'result'}):
                raise RuntimeError('owned Nitro RPC result refused')
            return value['result']
        finally:
            conn.close()
