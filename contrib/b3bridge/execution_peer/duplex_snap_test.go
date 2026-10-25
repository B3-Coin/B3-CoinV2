// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license.
package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"github.com/ethereum/go-ethereum/rlp"
	"math/big"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/ethereum/go-ethereum/common"
	"github.com/ethereum/go-ethereum/core"
	"github.com/ethereum/go-ethereum/core/types"
	"github.com/ethereum/go-ethereum/crypto"
	eth "github.com/ethereum/go-ethereum/eth/protocols/eth"
	snap "github.com/ethereum/go-ethereum/eth/protocols/snap"
	"github.com/ethereum/go-ethereum/log"
	"github.com/ethereum/go-ethereum/p2p"
)

type delayedStatusRW struct {
	p2p.MsgReadWriter
	ctx   context.Context
	reply <-chan struct{}
}

func (d delayedStatusRW) WriteMsg(m p2p.Msg) error {
	if m.Code == eth.StatusMsg {
		select {
		case <-d.reply:
		case <-d.ctx.Done():
			return d.ctx.Err()
		}
	}
	return d.MsgReadWriter.WriteMsg(m)
}

// The server delays its ETH status until the client answers an inbound SNAP
// request. Its ETH reader still drains the client's status. An availability
// client that waits for ETH readiness before reading SNAP deadlocks here.
func TestWitnessHandlesEarlyInboundSnapWithoutBypassingETHGate(t *testing.T) {
	earlyInboundFixture(t, false)
}
func TestCombinedExactHeaderAndSnapUseSameConnection(t *testing.T) { earlyInboundFixture(t, true) }
func earlyInboundFixture(t *testing.T, combined bool) {
	req, _, _, _, account, proof, storage := witnessFixture(t)
	stateRoot := req.StateRoot
	var rawHeader []byte
	if combined {
		header := &types.Header{Number: big.NewInt(1), Difficulty: big.NewInt(0), Root: common.HexToHash(stateRoot), Time: 1792940000}
		var err error
		rawHeader, err = rlp.EncodeToBytes(header)
		if err != nil {
			t.Fatal(err)
		}
		req.ExecutionHash = crypto.Keccak256Hash(rawHeader).Hex()
		req.StateRoot = ""
	}

	key, err := crypto.GenerateKey()
	if err != nil {
		t.Fatal(err)
	}
	chain := genesisChain{core.DefaultSepoliaGenesisBlock().ToBlock()}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	replied := make(chan struct{})
	var replyOnce sync.Once
	var queries atomic.Uint64
	server := p2p.Server{Config: p2p.Config{PrivateKey: key, MaxPeers: 2, NoDiscovery: true, ListenAddr: "127.0.0.1:0", Logger: log.NewLogger(log.DiscardHandler()), Protocols: []p2p.Protocol{
		{Name: eth.ProtocolName, Version: eth.ETH72, Length: 22, Run: func(p *p2p.Peer, rw p2p.MsgReadWriter) error {
			peer := eth.NewPeer(eth.ETH72, p, delayedStatusRW{rw, ctx, replied}, nil, nil, chain.Config())
			defer peer.Close()
			if err := peer.Handshake(networkID, chain, eth.BlockRangeUpdatePacket{LatestBlockHash: chain.block.Hash()}); err != nil {
				return err
			}
			for {
				m, err := rw.ReadMsg()
				if err != nil {
					return err
				}
				if m.Code == eth.GetBlockHeadersMsg {
					if !combined {
						return errors.New("unexpected combined header query")
					}
					var query eth.GetBlockHeadersPacket
					if m.Decode(&query) != nil || query.Amount != 1 || query.Origin.Hash.Hex() != req.ExecutionHash {
						return errors.New("wrong bound header query")
					}
					if err = p2p.Send(rw, eth.BlockHeadersMsg, &eth.BlockHeadersRLPPacket{RequestId: query.RequestId, BlockHeadersRLPResponse: []rlp.RawValue{rawHeader}}); err != nil {
						return err
					}
				} else if err = m.Discard(); err != nil {
					return err
				}
			}
		}},
		{Name: snap.ProtocolName, Version: snap.SNAP2, Length: 10, Run: func(p *p2p.Peer, rw p2p.MsgReadWriter) error {
			if err := p2p.Send(rw, snap.GetAccountRangeMsg, &snap.GetAccountRangePacket{ID: 42}); err != nil {
				return err
			}
			m, err := rw.ReadMsg()
			if err != nil {
				return err
			}
			var empty snap.AccountRangePacket
			if m.Code != snap.AccountRangeMsg || m.Decode(&empty) != nil || empty.ID != 42 || len(empty.Accounts) != 0 || len(empty.Proof) != 0 {
				return errors.New("early inbound request was not answered with unavailable data")
			}
			replyOnce.Do(func() { close(replied) })
			m, err = rw.ReadMsg()
			if err != nil {
				return err
			}
			var a snap.GetAccountRangePacket
			if m.Code != snap.GetAccountRangeMsg || m.Decode(&a) != nil {
				return errors.New("unexpected own account query")
			}
			queries.Add(1)
			if err = p2p.Send(rw, snap.AccountRangeMsg, &snap.AccountRangePacket{ID: a.ID, Accounts: []*snap.AccountData{{Hash: a.Origin, Body: types.SlimAccountRLP(account)}}, Proof: proof}); err != nil {
				return err
			}
			// A second inbound request interleaved with the storage proof must also be
			// answered, without misclassifying it as the outstanding proof response.
			if err = p2p.Send(rw, snap.GetByteCodesMsg, &snap.GetByteCodesPacket{ID: 43}); err != nil {
				return err
			}
			var q snap.GetStorageRangesPacket
			gotQuery, gotReply := false, false
			for !gotQuery || !gotReply {
				m, err = rw.ReadMsg()
				if err != nil {
					return err
				}
				switch m.Code {
				case snap.GetStorageRangesMsg:
					if err = m.Decode(&q); err != nil {
						return err
					}
					gotQuery = true
					queries.Add(1)
				case snap.ByteCodesMsg:
					var e snap.ByteCodesPacket
					if m.Decode(&e) != nil || e.ID != 43 || len(e.Codes) != 0 {
						return errors.New("interleaved unavailable reply invalid")
					}
					gotReply = true
				default:
					return errors.New("unexpected interleaved SNAP message")
				}
			}
			return p2p.Send(rw, snap.StorageRangesMsg, &snap.StorageRangesPacket{ID: q.ID, Slots: [][]*snap.StorageData{{{Hash: common.BytesToHash(q.Origin), Body: []byte{7}}}}, Proof: storage})
		}},
	}}}
	if err = server.Start(); err != nil {
		t.Fatal(err)
	}
	defer server.Stop()
	value, err := collectWitness(ctx, req, 1, server.Self().URLv4())
	if err != nil || !value.Passed || value.Handshakes != 1 || queries.Load() != 2 || value.Authority || value.Mainnet {
		t.Fatal("early/interleaved SNAP request blocked authenticated proof queries", err, value)
	}
	if combined && (value.ExecutionHash != req.ExecutionHash || value.StateRoot != stateRoot || value.Header != "0x"+common.Bytes2Hex(rawHeader)) {
		t.Fatal("combined header preimage/root binding failed")
	}

}

func TestDuplexReaderTerminationAndUnsolicitedResponsesReleaseGate(t *testing.T) {
	for _, unsolicited := range []bool{false, true} {
		t.Run(fmt.Sprint(unsolicited), func(t *testing.T) {
			a, b := p2p.MsgPipe()
			defer a.Close()
			defer b.Close()
			ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
			defer cancel()
			duplex := newWitnessDuplex(ctx, a, snap.SNAP2)
			if unsolicited {
				go p2p.Send(b, snap.AccountRangeMsg, &snap.AccountRangePacket{ID: 99})
			} else {
				b.Close()
			}
			select {
			case <-duplex.done:
				if duplex.Err() == nil {
					t.Fatal("reader ended without a failure signal")
				}
			case <-time.After(500 * time.Millisecond):
				t.Fatal("pre-ETH reader failure retained the connection")
			}
			if len(duplex.incoming) != 0 {
				t.Fatal("unsolicited response entered the pre-gate queue")
			}
		})
	}
}
func TestDuplexKeepsCompleteReplyBeforeRemoteEOF(t *testing.T) {
	a, b := p2p.MsgPipe()
	defer a.Close()
	defer b.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	duplex := newWitnessDuplex(ctx, a, snap.SNAP2)
	complete := make(chan struct{})
	go func() {
		m, err := b.ReadMsg()
		if err != nil {
			return
		}
		var q snap.GetAccountRangePacket
		if m.Decode(&q) != nil {
			return
		}
		p2p.Send(b, snap.AccountRangeMsg, &snap.AccountRangePacket{ID: q.ID})
		b.Close()
		close(complete)
	}()
	if err := p2p.Send(duplex, snap.GetAccountRangeMsg, &snap.GetAccountRangePacket{ID: 3}); err != nil {
		t.Fatal(err)
	}
	<-complete
	<-duplex.done
	m, err := duplex.ReadMsg()
	if err != nil {
		t.Fatal("complete proof response discarded after EOF", err)
	}
	var q snap.AccountRangePacket
	if m.Decode(&q) != nil || q.ID != 3 {
		t.Fatal("wrong queued reply")
	}
}

func TestCombinedRequestRequiresOneUntrustedRootSource(t *testing.T) {
	req, _, _, _, _, _, _ := witnessFixture(t)
	req.ExecutionHash = "0x" + strings.Repeat("ab", 32)
	raw, _ := json.Marshal(req)
	if _, err := decodeWitnessRequest(raw); err == nil {
		t.Fatal("caller supplied both execution hash and root")
	}
	req.StateRoot = ""
	raw, _ = json.Marshal(req)
	if _, err := decodeWitnessRequest(raw); err != nil {
		t.Fatal("exact execution hash availability request refused", err)
	}
	req.ExecutionHash = ""
	raw, _ = json.Marshal(req)
	if _, err := decodeWitnessRequest(raw); err == nil {
		t.Fatal("missing root source accepted")
	}
}
