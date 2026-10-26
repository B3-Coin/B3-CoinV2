// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license.
package main

import (
	"bytes"
	"context"
	"encoding/hex"
	"fmt"
	"github.com/ethereum/go-ethereum/common"
	"github.com/ethereum/go-ethereum/core/types"
	eth "github.com/ethereum/go-ethereum/eth/protocols/eth"
	"github.com/ethereum/go-ethereum/p2p"
	"github.com/ethereum/go-ethereum/rlp"
	"io"
	"testing"
	"time"
)

type unavailableFixture struct {
	message p2p.Msg
	writes  int
}

func (f *unavailableFixture) ReadMsg() (p2p.Msg, error) { return p2p.Msg{}, io.EOF }
func (f *unavailableFixture) WriteMsg(m p2p.Msg) error {
	b, e := io.ReadAll(m.Payload)
	if e != nil {
		return e
	}
	f.message = p2p.Msg{Code: m.Code, Size: m.Size, Payload: bytes.NewReader(b)}
	f.writes++
	return nil
}
func unavailableMessage(t *testing.T, code uint64, p any) p2p.Msg {
	t.Helper()
	b, e := rlp.EncodeToBytes(p)
	if e != nil {
		t.Fatal(e)
	}
	return p2p.Msg{Code: code, Size: uint32(len(b)), Payload: bytes.NewReader(b)}
}
func TestUnavailableETHVersionsAndExactWire(t *testing.T) {
	hashes := []common.Hash{common.HexToHash("0x01"), common.HexToHash("0x02")}
	mask := types.NewCustodyBitmap([]uint64{0, 3, 127})
	for _, version := range []uint{eth.ETH69, eth.ETH70, eth.ETH71, eth.ETH72} {
		for _, code := range []uint64{eth.GetReceiptsMsg, eth.GetBlockAccessListsMsg, eth.GetCellsMsg} {
			t.Run(fmt.Sprint(version, "/", code), func(t *testing.T) {
				var packet any
				switch code {
				case eth.GetReceiptsMsg:
					if version == eth.ETH69 {
						packet = &eth.GetReceiptsPacket69{RequestId: ^uint64(0), GetReceiptsRequest: hashes}
					} else {
						packet = &eth.GetReceiptsPacket70{RequestId: ^uint64(0), FirstBlockReceiptIndex: 42, GetReceiptsRequest: hashes}
					}
				case eth.GetBlockAccessListsMsg:
					packet = &eth.GetBlockAccessListsPacket{RequestId: ^uint64(0), GetBlockAccessListsRequest: hashes}
				case eth.GetCellsMsg:
					packet = &eth.GetCellsRequestPacket{RequestId: ^uint64(0), Hashes: hashes, Mask: mask}
				}
				f := new(unavailableFixture)
				handled, e := replyUnavailableETHData(unavailableMessage(t, code, packet), f, version)
				supported := code == eth.GetReceiptsMsg || (code == eth.GetBlockAccessListsMsg && version >= eth.ETH71) || (code == eth.GetCellsMsg && version == eth.ETH72)
				if !handled || (!supported && (e == nil || f.writes != 0)) {
					t.Fatal("version gate")
				}
				if !supported {
					return
				}
				if e != nil || f.writes != 1 {
					t.Fatal(e)
				}
				if err := validateUnavailableETHReply(&f.message); err != nil {
					t.Fatal(err)
				}
				switch code {
				case eth.GetReceiptsMsg:
					if version == eth.ETH69 {
						var p eth.ReceiptsPacket69
						if f.message.Decode(&p) != nil || p.RequestId != ^uint64(0) || p.List.Len() != 0 {
							t.Fatal("receipt69")
						}
					} else {
						var p eth.ReceiptsPacket70
						if f.message.Decode(&p) != nil || p.RequestId != ^uint64(0) || p.List.Len() != 0 || p.LastBlockIncomplete {
							t.Fatal("receipt70")
						}
					}
				case eth.GetBlockAccessListsMsg:
					var p eth.BlockAccessListPacket
					if f.message.Decode(&p) != nil || p.RequestId != ^uint64(0) || p.List.Len() != 2 {
						t.Fatal("BAL")
					}
					items, _ := p.List.Items()
					for _, item := range items {
						if !bytes.Equal(item, rlp.EmptyString) {
							t.Fatal("BAL marker")
						}
					}
				case eth.GetCellsMsg:
					var p eth.CellsPacket
					if f.message.Decode(&p) != nil || p.RequestId != ^uint64(0) || p.Mask != mask || len(p.Hashes) != 0 || p.Cells.Len() != 0 {
						t.Fatal("cell reply")
					}
				}
			})
		}
	}
	f := new(unavailableFixture)
	if handled, e := replyUnavailableETHData(p2p.Msg{Code: eth.BlockHeadersMsg}, f, eth.ETH72); handled || e != nil || f.writes != 0 {
		t.Fatal("unknown/response handled")
	}
}
func TestUnavailableETHMalformedAndBounded(t *testing.T) {
	for _, version := range []uint{68, 73} {
		if _, e := replyUnavailableETHData(p2p.Msg{Code: eth.GetReceiptsMsg}, new(unavailableFixture), version); e == nil {
			t.Fatal("unsupported version")
		}
	}
	for _, code := range []uint64{eth.GetReceiptsMsg, eth.GetBlockAccessListsMsg, eth.GetCellsMsg} {
		for _, b := range [][]byte{{0xc0}, {0xc2, 0x01, 0xc0, 0x00}} {
			f := new(unavailableFixture)
			if _, e := replyUnavailableETHData(p2p.Msg{Code: code, Size: uint32(len(b)), Payload: bytes.NewReader(b)}, f, eth.ETH72); e == nil || f.writes != 0 {
				t.Fatal("malformed/trailing request")
			}
		}
	}
	for _, m := range []p2p.Msg{{Code: eth.GetReceiptsMsg, Size: maxMessageBytes + 1, Payload: bytes.NewReader(nil)}, {Code: eth.GetReceiptsMsg, Size: 20, Payload: bytes.NewReader([]byte{0xc0})}} {
		if _, e := replyUnavailableETHData(m, new(unavailableFixture), eth.ETH72); e == nil {
			t.Fatal("frame bounds")
		}
	}
	q := eth.GetBlockAccessListsPacket{RequestId: 9, GetBlockAccessListsRequest: make([]common.Hash, 1025)}
	f := new(unavailableFixture)
	if _, e := replyUnavailableETHData(unavailableMessage(t, eth.GetBlockAccessListsMsg, &q), f, eth.ETH72); e != nil {
		t.Fatal(e)
	}
	var r eth.BlockAccessListPacket
	if f.message.Decode(&r) != nil || r.List.Len() != 1024 {
		t.Fatal("BAL response bound")
	}
}
func TestUnavailableETHOutboundPayloadGuard(t *testing.T) {
	var validBAL rlp.RawList[rlp.RawValue]
	validBAL.AppendRaw(rlp.EmptyString)
	var badBAL rlp.RawList[rlp.RawValue]
	badBAL.AppendRaw(rlp.EmptyList)
	nonempty := eth.ReceiptsPacket70{RequestId: 1}
	if e := nonempty.List.Append(eth.NewReceiptList([]*types.Receipt{{Status: 1}})); e != nil {
		t.Fatal(e)
	}
	good := []p2p.Msg{unavailableMessage(t, eth.ReceiptsMsg, &eth.ReceiptsPacket69{RequestId: 1}), unavailableMessage(t, eth.ReceiptsMsg, &eth.ReceiptsPacket70{RequestId: 1}), unavailableMessage(t, eth.BlockAccessListsMsg, &eth.BlockAccessListPacket{RequestId: 1, List: validBAL}), unavailableMessage(t, eth.CellsMsg, &eth.CellsPacket{RequestId: 1})}
	for _, original := range good {
		b, _ := io.ReadAll(original.Payload)
		for _, receipt := range []bool{false, true} {
			inner := new(unavailableFixture)
			var rw p2p.MsgWriter = &boundedRW{MsgReadWriter: inner}
			if receipt {
				rw = &boundedReceiptRW{MsgReadWriter: inner}
			}
			m := p2p.Msg{Code: original.Code, Size: uint32(len(b)), Payload: bytes.NewReader(b)}
			if rw.WriteMsg(m) != nil || inner.writes != 1 {
				t.Fatal("honest reply refused")
			}
		}
	}
	bad := []p2p.Msg{unavailableMessage(t, eth.ReceiptsMsg, &nonempty), unavailableMessage(t, eth.ReceiptsMsg, &eth.ReceiptsPacket70{LastBlockIncomplete: true}), unavailableMessage(t, eth.BlockAccessListsMsg, &eth.BlockAccessListPacket{List: badBAL}), unavailableMessage(t, eth.CellsMsg, &eth.CellsPacket{Hashes: []common.Hash{{1}}})}
	for _, original := range bad {
		b, _ := io.ReadAll(original.Payload)
		for _, receipt := range []bool{false, true} {
			inner := new(unavailableFixture)
			var rw p2p.MsgWriter = &boundedRW{MsgReadWriter: inner}
			if receipt {
				rw = &boundedReceiptRW{MsgReadWriter: inner}
			}
			m := p2p.Msg{Code: original.Code, Size: uint32(len(b)), Payload: bytes.NewReader(b)}
			if rw.WriteMsg(m) == nil || inner.writes != 0 {
				t.Fatal("nonempty/partial data escaped")
			}
		}
	}
	for _, code := range []uint64{eth.GetBlockAccessListsMsg, eth.GetCellsMsg, eth.TransactionsMsg, eth.PooledTransactionsMsg, eth.GetPooledTransactionsMsg} {
		if (&boundedRW{MsgReadWriter: new(unavailableFixture)}).WriteMsg(p2p.Msg{Code: code}) == nil {
			t.Fatal("new request/transaction code escaped")
		}
	}
}
func TestExactHeaderSurvivesUnavailableETHQueries(t *testing.T) {
	client, server := p2p.MsgPipe()
	defer client.Close()
	defer server.Close()
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	_, header := receiptFixture()
	raw, e := rlp.EncodeToBytes(&header)
	if e != nil {
		t.Fatal(e)
	}
	// Same exact bytes/hash still decide success after the availability replies.
	errCh := make(chan error, 1)
	go func() {
		m, e := server.ReadMsg()
		if e != nil {
			errCh <- e
			return
		}
		var q eth.GetBlockHeadersPacket
		if m.Decode(&q) != nil {
			errCh <- fmt.Errorf("header query")
			return
		}
		packets := []struct {
			code     uint64
			packet   any
			response uint64
		}{{eth.GetReceiptsMsg, &eth.GetReceiptsPacket70{RequestId: 7}, eth.ReceiptsMsg}, {eth.GetBlockAccessListsMsg, &eth.GetBlockAccessListsPacket{RequestId: 8, GetBlockAccessListsRequest: []common.Hash{{1}}}, eth.BlockAccessListsMsg}, {eth.GetCellsMsg, &eth.GetCellsRequestPacket{RequestId: 9}, eth.CellsMsg}}
		for _, p := range packets {
			if e = p2p.Send(server, p.code, p.packet); e != nil {
				errCh <- e
				return
			}
			m, e = server.ReadMsg()
			if e != nil || m.Code != p.response {
				errCh <- fmt.Errorf("unavailable response")
				return
			}
			if e = validateUnavailableETHReply(&m); e != nil {
				errCh <- e
				return
			}
		}
		errCh <- p2p.Send(server, eth.BlockHeadersMsg, &eth.BlockHeadersRLPPacket{RequestId: q.RequestId, BlockHeadersRLPResponse: []rlp.RawValue{raw}})
	}()
	got, e := witnessExecutionHeader(ctx, &boundedRW{MsgReadWriter: client}, header.Hash(), eth.ETH72)
	if e != nil || !bytes.Equal(got, raw) {
		t.Fatalf("header after queries: %v %s", e, hex.EncodeToString(got))
	}
	if e = <-errCh; e != nil {
		t.Fatal(e)
	}
}
