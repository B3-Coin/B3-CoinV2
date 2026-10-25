// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license.
package main

import (
	"bytes"
	"context"
	"math/big"
	"strings"
	"testing"
	"time"

	"github.com/ethereum/go-ethereum/common"
	"github.com/ethereum/go-ethereum/core"
	"github.com/ethereum/go-ethereum/core/types"
	"github.com/ethereum/go-ethereum/crypto"
	eth "github.com/ethereum/go-ethereum/eth/protocols/eth"
	"github.com/ethereum/go-ethereum/p2p"
	"github.com/ethereum/go-ethereum/p2p/enode"
	"github.com/ethereum/go-ethereum/params"
	"github.com/ethereum/go-ethereum/rlp"
)

func testHeader(t *testing.T) ([]byte, common.Hash) {
	t.Helper()
	h := &types.Header{Number: big.NewInt(1), Difficulty: big.NewInt(0), GasLimit: 30000000, Time: 1900000000,
		Root: common.HexToHash("0x11"), TxHash: types.EmptyTxsHash, ReceiptHash: types.EmptyReceiptsHash}
	b, err := rlp.EncodeToBytes(h)
	if err != nil {
		t.Fatal(err)
	}
	return b, crypto.Keccak256Hash(b)
}
func TestStrictHash(t *testing.T) {
	good := "0x" + strings.Repeat("ab", 32)
	if _, err := parseHash(good); err != nil {
		t.Fatal(err)
	}
	for _, bad := range []string{"", good[2:], strings.ToUpper(good), good + "00", "0x" + strings.Repeat("0", 64), "0x" + strings.Repeat("gg", 32)} {
		if _, err := parseHash(bad); err == nil {
			t.Fatalf("accepted malformed hash %q", bad)
		}
	}
}
func TestExactRawHeaderOnly(t *testing.T) {
	raw, hash := testHeader(t)
	p := eth.BlockHeadersRLPPacket{RequestId: 7, BlockHeadersRLPResponse: []rlp.RawValue{raw}}
	got, err := acceptHeader(p, 7, hash)
	if err != nil || !bytes.Equal(got, raw) {
		t.Fatal("valid header refused", err)
	}
	got[0] ^= 1
	if bytes.Equal(got, raw) {
		t.Fatal("output aliases input")
	}
	if _, err = acceptHeader(p, 8, hash); err == nil {
		t.Fatal("wrong request admitted")
	}
	if _, err = acceptHeader(p, 7, common.HexToHash("0x22")); err == nil {
		t.Fatal("wrong header hash admitted")
	}
	p.BlockHeadersRLPResponse = append(p.BlockHeadersRLPResponse, raw)
	if _, err = acceptHeader(p, 7, hash); err == nil {
		t.Fatal("extra header admitted")
	}
	p.BlockHeadersRLPResponse = nil
	if _, err = acceptHeader(p, 7, hash); err == nil {
		t.Fatal("missing header admitted")
	}
}
func TestMalformedAndOversizeHeaders(t *testing.T) {
	for _, raw := range [][]byte{{0xc0}, {0x80}, bytes.Repeat([]byte{1}, maxHeaderBytes+1)} {
		p := eth.BlockHeadersRLPPacket{RequestId: 1, BlockHeadersRLPResponse: []rlp.RawValue{raw}}
		if _, err := acceptHeader(p, 1, crypto.Keccak256Hash(raw)); err == nil {
			t.Fatal("malformed/oversize raw accepted")
		}
	}
}
func TestNoAuthorityOrMainnetResult(t *testing.T) {
	r := baseResult()
	if r.Mainnet || r.Authority || !r.AvailabilityOnly || !r.NativeRequired || r.ChainID != 11155111 || r.Genesis != params.SepoliaGenesisHash.Hex() {
		t.Fatal("availability minted authority")
	}
	if core.DefaultSepoliaGenesisBlock().ToBlock().Hash() != params.SepoliaGenesisHash {
		t.Fatal("wrong genesis")
	}
}
func TestOutboundTransactionsDenied(t *testing.T) {
	a, b := p2p.MsgPipe()
	defer a.Close()
	defer b.Close()
	rw := &boundedRW{MsgReadWriter: a}
	for _, code := range []uint64{eth.TransactionsMsg, eth.NewPooledTransactionHashesMsg, eth.GetPooledTransactionsMsg, 99} {
		if err := rw.WriteMsg(p2p.Msg{Code: code}); err == nil {
			t.Fatal("outbound write method admitted")
		}
	}
}
func TestPeerByteAndMessageBudgets(t *testing.T) {
	a, b := p2p.MsgPipe()
	defer a.Close()
	defer b.Close()
	rw := &boundedRW{MsgReadWriter: a, bytes: maxPeerBytes}
	go b.WriteMsg(p2p.Msg{Code: eth.BlockHeadersMsg, Size: 1, Payload: bytes.NewReader([]byte{0})})
	if _, err := rw.ReadMsg(); err == nil {
		t.Fatal("aggregate budget exceeded")
	}
}
func TestMaintainedGethHandshake(t *testing.T) {
	a, b := p2p.MsgPipe()
	defer a.Close()
	defer b.Close()
	chain := genesisChain{core.DefaultSepoliaGenesisBlock().ToBlock()}
	p1 := eth.NewPeer(eth.ETH69, p2p.NewPeer(enode.ID{1}, "fixture1", nil), a, nil, nil, params.SepoliaChainConfig)
	p2 := eth.NewPeer(eth.ETH69, p2p.NewPeer(enode.ID{2}, "fixture2", nil), b, nil, nil, params.SepoliaChainConfig)
	defer p1.Close()
	defer p2.Close()
	rangeMsg := eth.BlockRangeUpdatePacket{LatestBlockHash: chain.block.Hash()}
	err := make(chan error, 2)
	go func() { err <- p1.Handshake(networkID, chain, rangeMsg) }()
	go func() { err <- p2.Handshake(networkID, chain, rangeMsg) }()
	for i := 0; i < 2; i++ {
		select {
		case e := <-err:
			if e != nil {
				t.Fatal(e)
			}
		case <-time.After(3 * time.Second):
			t.Fatal("handshake blocked")
		}
	}
}
func TestPublicDialerRefusesPrivateTargets(t *testing.T) {
	key, err := crypto.GenerateKey()
	if err != nil {
		t.Fatal(err)
	}
	n := enode.NewV4(&key.PublicKey, []byte{127, 0, 0, 1}, 30303, 30303)
	if _, err = (publicDialer{}).Dial(context.Background(), n); err == nil {
		t.Fatal("public mode dialed loopback")
	}
	n = enode.NewV4(&key.PublicKey, []byte{192, 168, 1, 1}, 30303, 30303)
	if _, err = (publicDialer{}).Dial(context.Background(), n); err == nil {
		t.Fatal("public mode dialed private network")
	}
}

func TestActualHeaderRequestExchange(t *testing.T) {
	a, b := p2p.MsgPipe()
	defer a.Close()
	defer b.Close()
	chain := genesisChain{core.DefaultSepoliaGenesisBlock().ToBlock()}
	raw, want := testHeader(t)
	server := eth.NewPeer(eth.ETH69, p2p.NewPeer(enode.ID{3}, "fixture-server", nil), b, nil, nil, params.SepoliaChainConfig)
	defer server.Close()
	errors := make(chan error, 2)
	output := make(chan result, 1)
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	go func() {
		if err := server.Handshake(networkID, chain, eth.BlockRangeUpdatePacket{LatestBlockHash: chain.block.Hash()}); err != nil {
			errors <- err
			return
		}
		m, err := b.ReadMsg()
		if err != nil {
			errors <- err
			return
		}
		var request eth.GetBlockHeadersPacket
		if err = m.Decode(&request); err != nil {
			errors <- err
			return
		}
		if m.Code != eth.GetBlockHeadersMsg || request.Amount != 1 || request.Origin.Hash != want {
			errors <- context.Canceled
			return
		}
		errors <- p2p.Send(b, eth.BlockHeadersMsg, &eth.BlockHeadersRLPPacket{RequestId: request.RequestId, BlockHeadersRLPResponse: []rlp.RawValue{raw}})
	}()
	go func() {
		errors <- readPeer(ctx, p2p.NewPeer(enode.ID{4}, "fixture-client", nil), a, eth.ETH69, chain, want, false, output, new(counters))
	}()
	for i := 0; i < 2; i++ {
		select {
		case err := <-errors:
			if err != nil {
				t.Fatal(err)
			}
		case <-ctx.Done():
			t.Fatal("header exchange timed out")
		}
	}
	select {
	case v := <-output:
		if !v.Passed || v.Authority || v.Mainnet || v.Header != "0x"+common.Bytes2Hex(raw) {
			t.Fatal("wrong availability result")
		}
	default:
		t.Fatal("no returned header")
	}
}
