// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license.
package main

import (
	"context"
	"errors"
	"math/big"
	"testing"
	"time"

	"github.com/ethereum/go-ethereum/core"
	"github.com/ethereum/go-ethereum/core/forkid"
	"github.com/ethereum/go-ethereum/core/types"
	"github.com/ethereum/go-ethereum/crypto"
	eth "github.com/ethereum/go-ethereum/eth/protocols/eth"
	snap "github.com/ethereum/go-ethereum/eth/protocols/snap"
	"github.com/ethereum/go-ethereum/log"
	"github.com/ethereum/go-ethereum/p2p"
	"github.com/ethereum/go-ethereum/p2p/enode"
	"github.com/ethereum/go-ethereum/rlp"
)

type activeForkTestChain struct {
	genesisChain
	head *types.Header
}

func (c activeForkTestChain) CurrentHeader() *types.Header { return c.head }

func TestMaintainedForkFilterAcceptsGenesisSyncPeerAtActiveFork(t *testing.T) {
	old := genesisChain{core.DefaultSepoliaGenesisBlock().ToBlock()}
	head := old.block.Header()
	head.Number = big.NewInt(100000000)
	head.Time = 1792940000
	current := activeForkTestChain{old, head}
	id := forkid.NewID(old.Config(), old.block, 0, old.block.Time())
	if err := forkid.NewFilter(current)(id); err != nil {
		t.Fatal("maintained current-fork peer rejects valid genesis-sync fork ID", err)
	}
	id.Hash[0] ^= 1
	if forkid.NewFilter(current)(id) == nil {
		t.Fatal("unknown fork ID accepted")
	}
	a, b := p2p.MsgPipe()
	defer a.Close()
	defer b.Close()
	p1 := eth.NewPeer(eth.ETH72, p2p.NewPeer(enode.ID{1}, "old", nil), a, nil, nil, old.Config())
	defer p1.Close()
	p2 := eth.NewPeer(eth.ETH72, p2p.NewPeer(enode.ID{2}, "current", nil), b, nil, nil, old.Config())
	defer p2.Close()
	done := make(chan error, 2)
	go func() {
		done <- p1.Handshake(networkID, old, eth.BlockRangeUpdatePacket{LatestBlockHash: old.block.Hash()})
	}()
	go func() {
		done <- p2.Handshake(networkID, current, eth.BlockRangeUpdatePacket{LatestBlock: 100000000, LatestBlockHash: head.Hash()})
	}()
	for i := 0; i < 2; i++ {
		select {
		case err := <-done:
			if err != nil {
				t.Fatal(err)
			}
		case <-time.After(3 * time.Second):
			t.Fatal("active-fork handshake stalled")
		}
	}
}
func TestUnavailableSnapResponsesKeepRequestIDs(t *testing.T) {
	a, b := p2p.MsgPipe()
	defer a.Close()
	defer b.Close()
	done := make(chan error, 1)
	go func() { done <- serveUnavailableSnap(a, snap.SNAP2) }()
	cases := []struct {
		request  uint64
		packet   any
		response uint64
	}{
		{snap.GetAccountRangeMsg, &snap.GetAccountRangePacket{ID: 9}, snap.AccountRangeMsg},
		{snap.GetStorageRangesMsg, &snap.GetStorageRangesPacket{ID: 9}, snap.StorageRangesMsg},
		{snap.GetByteCodesMsg, &snap.GetByteCodesPacket{ID: 9}, snap.ByteCodesMsg},
		{snap.GetTrieNodesMsg, &snap.GetTrieNodesPacket{ID: 9}, snap.TrieNodesMsg},
		{snap.GetAccessListsMsg, &snap.GetAccessListsPacket{ID: 9}, snap.AccessListsMsg},
	}
	for _, c := range cases {
		if err := p2p.Send(b, c.request, c.packet); err != nil {
			t.Fatal(err)
		}
		msg, err := b.ReadMsg()
		if err != nil {
			t.Fatal(err)
		}
		if msg.Code != c.response {
			t.Fatal("wrong empty response")
		}
		var fields []rlp.RawValue
		if err = msg.Decode(&fields); err != nil || len(fields) < 2 {
			t.Fatal("bad response", err)
		}
		var id uint64
		if err = rlp.DecodeBytes(fields[0], &id); err != nil || id != 9 {
			t.Fatal("lost request ID")
		}
		for _, field := range fields[1:] {
			var data []rlp.RawValue
			if rlp.DecodeBytes(field, &data) != nil || len(data) != 0 {
				t.Fatal("unavailable data reply invented state")
			}
		}
	}
	b.Close()
	if err := <-done; err == nil {
		t.Fatal("closed pipe not reported")
	}
}
func TestHeaderCollectorNegotiatesSnapExtensionWithMaintainedPeer(t *testing.T) {
	raw, want := testHeader(t)
	key, err := crypto.GenerateKey()
	if err != nil {
		t.Fatal(err)
	}
	chain := genesisChain{core.DefaultSepoliaGenesisBlock().ToBlock()}
	snapReady := make(chan struct{})
	server := p2p.Server{Config: p2p.Config{PrivateKey: key, MaxPeers: 2, NoDiscovery: true, ListenAddr: "127.0.0.1:0", Logger: log.NewLogger(log.DiscardHandler()), Protocols: []p2p.Protocol{
		{Name: snap.ProtocolName, Version: snap.SNAP2, Length: 10, Run: func(p *p2p.Peer, rw p2p.MsgReadWriter) error {
			close(snapReady)
			if err := p2p.Send(rw, snap.GetAccountRangeMsg, &snap.GetAccountRangePacket{ID: 42}); err != nil {
				return err
			}
			m, err := rw.ReadMsg()
			if err != nil {
				return err
			}
			var q snap.AccountRangePacket
			if m.Code != snap.AccountRangeMsg {
				return errors.New("incorrect SNAP capability offset")
			}
			if err = m.Decode(&q); err != nil || q.ID != 42 || len(q.Accounts) != 0 || len(q.Proof) != 0 {
				return errors.New("invented unavailable state")
			}
			return serveUnavailableSnap(rw, snap.SNAP2)
		}},
		{Name: eth.ProtocolName, Version: eth.ETH72, Length: 22, Run: func(p *p2p.Peer, rw p2p.MsgReadWriter) error {
			select {
			case <-snapReady:
			case <-time.After(3 * time.Second):
				return errors.New("SNAP extension not negotiated")
			}
			peer := eth.NewPeer(eth.ETH72, p, rw, nil, nil, chain.Config())
			defer peer.Close()
			if err := peer.Handshake(networkID, chain, eth.BlockRangeUpdatePacket{LatestBlockHash: chain.block.Hash()}); err != nil {
				return err
			}
			m, err := rw.ReadMsg()
			if err != nil {
				return err
			}
			var q eth.GetBlockHeadersPacket
			if m.Code != eth.GetBlockHeadersMsg {
				return errors.New("incorrect ETH capability offset")
			}
			if err = m.Decode(&q); err != nil {
				return err
			}
			return p2p.Send(rw, eth.BlockHeadersMsg, &eth.BlockHeadersRLPPacket{RequestId: q.RequestId, BlockHeadersRLPResponse: []rlp.RawValue{raw}})
		}},
	}}}
	serverErrors := make(chan error, 8)
	for i := range server.Protocols {
		run := server.Protocols[i].Run
		server.Protocols[i].Run = func(p *p2p.Peer, rw p2p.MsgReadWriter) error { err := run(p, rw); serverErrors <- err; return err }
	}
	if err = server.Start(); err != nil {
		t.Fatal(err)
	}
	defer server.Stop()
	ctx, cancel := context.WithTimeout(context.Background(), 4*time.Second)
	defer cancel()
	value, err := collect(ctx, want, false, 1, server.Self().URLv4())
	if err != nil || !value.Passed || value.Protocol != eth.ETH72 || value.Authority || value.Mainnet {
		for len(serverErrors) > 0 {
			t.Log("owned server error:", <-serverErrors)
		}
		t.Fatal("real maintained ETH72/SNAP2 negotiation failed", err, value)
	}
}
func TestPeerDropClassificationDoesNotExposePeerText(t *testing.T) {
	if peerDropClass("remote 1.2.3.4: too many peers") != "too_many_peers" {
		t.Fatal("drop classification lost")
	}
	if peerDropClass("secret endpoint arbitrary detail") != "other" {
		t.Fatal("private peer detail escaped")
	}
}
