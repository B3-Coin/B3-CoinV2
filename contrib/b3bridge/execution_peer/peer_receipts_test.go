// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license.
package main

import (
	"bytes"
	"context"
	"errors"
	"github.com/ethereum/go-ethereum/common"
	"github.com/ethereum/go-ethereum/core/types"
	"github.com/ethereum/go-ethereum/crypto"
	eth "github.com/ethereum/go-ethereum/eth/protocols/eth"
	"github.com/ethereum/go-ethereum/p2p"
	"github.com/ethereum/go-ethereum/rlp"
	"github.com/ethereum/go-ethereum/trie"
	"io"
	"math/big"
	"testing"
	"time"
)

func receiptFixture() ([]*types.Receipt, types.Header) {
	receipts := []*types.Receipt{{Type: 0, Status: 1, CumulativeGasUsed: 21000, Logs: []*types.Log{{Address: common.HexToAddress("0x1234"), Topics: []common.Hash{common.HexToHash("0x42")}, Data: []byte{1, 2, 3}}}}, {Type: 2, Status: 1, CumulativeGasUsed: 42000}}
	for _, r := range receipts {
		r.Bloom = types.CreateBloom(r)
	}
	header := types.Header{ParentHash: common.HexToHash("0x11"), UncleHash: types.EmptyUncleHash, Root: common.HexToHash("0x22"), TxHash: common.HexToHash("0x33"), ReceiptHash: types.DeriveSha(types.Receipts(receipts), trie.NewStackTrie(nil)), Difficulty: new(big.Int), Number: big.NewInt(123), GasLimit: 30_000_000, GasUsed: 42000, Time: 1792942944, BaseFee: big.NewInt(1)}
	return receipts, header
}
func sendReceiptFixture(rw p2p.MsgReadWriter, version uint, mutation string, interleave bool) error {
	receipts, header := receiptFixture()
	if mutation == "wrong_number" {
		header.Number = big.NewInt(124)
	}
	if mutation == "wrong_root" {
		header.ReceiptHash = common.HexToHash("0xdead")
	}
	if mutation == "gas_backwards" {
		receipts[1].CumulativeGasUsed = 10
		header.ReceiptHash = types.DeriveSha(types.Receipts(receipts), trie.NewStackTrie(nil))
	}
	if mutation == "gas_final" {
		header.GasUsed++
	}
	if mutation == "empty_block" {
		receipts = nil
		header.ReceiptHash = types.EmptyReceiptsHash
		header.GasUsed = 0
	}
	if mutation == "unknown_type" {
		receipts[0].Type = 0x7f
		header.ReceiptHash = types.DeriveSha(eth.NewReceiptList(receipts).Derivable(), trie.NewStackTrie(nil))
	}
	msg, err := rw.ReadMsg()
	if err != nil {
		return err
	}
	var query eth.GetBlockHeadersPacket
	if msg.Code != eth.GetBlockHeadersMsg || msg.Decode(&query) != nil || query.GetBlockHeadersRequest == nil || query.Origin.Number != 123 || query.Amount != 1 {
		return errors.New("incorrect number header query")
	}
	if interleave {
		if version == eth.ETH69 {
			err = p2p.Send(rw, eth.GetReceiptsMsg, &eth.GetReceiptsPacket69{RequestId: 999})
		} else {
			err = p2p.Send(rw, eth.GetReceiptsMsg, &eth.GetReceiptsPacket70{RequestId: 999})
		}
		if err != nil {
			return err
		}
		msg, err = rw.ReadMsg()
		if err != nil {
			return err
		}
		if msg.Code != eth.ReceiptsMsg {
			return errors.New("wrong unavailable response code")
		}
		if version == eth.ETH69 {
			var empty eth.ReceiptsPacket69
			if msg.Decode(&empty) != nil || empty.RequestId != 999 || empty.List.Len() != 0 {
				return errors.New("wrong unavailable69 response")
			}
		} else {
			var empty eth.ReceiptsPacket70
			if msg.Decode(&empty) != nil || empty.RequestId != 999 || empty.LastBlockIncomplete || empty.List.Len() != 0 {
				return errors.New("wrong unavailable70 response")
			}
		}
	}
	raw, err := rlp.EncodeToBytes(&header)
	if err != nil {
		return err
	}
	id := query.RequestId
	if mutation == "wrong_header_id" {
		id++
	}
	if err = p2p.Send(rw, eth.BlockHeadersMsg, &eth.BlockHeadersRLPPacket{RequestId: id, BlockHeadersRLPResponse: []rlp.RawValue{raw}}); err != nil {
		return err
	}
	if mutation == "wrong_number" || mutation == "wrong_header_id" {
		return nil
	}
	pages := [][]*types.Receipt{receipts}
	if version >= eth.ETH70 && len(receipts) > 1 && mutation != "no_progress" {
		pages = [][]*types.Receipt{receipts[:1], receipts[1:]}
	}
	for page, items := range pages {
		msg, err = rw.ReadMsg()
		if err != nil {
			return err
		}
		var gotID, offset uint64
		var hashes []common.Hash
		if msg.Code != eth.GetReceiptsMsg {
			return errors.New("incorrect receipts query code")
		}
		if version == eth.ETH69 {
			var q eth.GetReceiptsPacket69
			if msg.Decode(&q) != nil {
				return errors.New("bad69 query")
			}
			gotID = q.RequestId
			hashes = q.GetReceiptsRequest
		} else {
			var q eth.GetReceiptsPacket70
			if msg.Decode(&q) != nil {
				return errors.New("bad70 query")
			}
			gotID = q.RequestId
			offset = q.FirstBlockReceiptIndex
			hashes = q.GetReceiptsRequest
		}
		if len(hashes) != 1 || hashes[0] != crypto.Keccak256Hash(raw) || offset != uint64(page) {
			return errors.New("receipt hash or partial index mismatch")
		}
		if mutation == "wrong_receipt_id" {
			gotID++
		}
		incomplete := page < len(pages)-1
		if mutation == "no_progress" {
			items = nil
			incomplete = true
		}
		if version == eth.ETH69 {
			p := eth.ReceiptsPacket69{RequestId: gotID}
			if err = p.List.Append(eth.NewReceiptList(items)); err != nil {
				return err
			}
			err = p2p.Send(rw, eth.ReceiptsMsg, &p)
		} else {
			p := eth.ReceiptsPacket70{RequestId: gotID, LastBlockIncomplete: incomplete}
			if err = p.List.Append(eth.NewReceiptList(items)); err != nil {
				return err
			}
			err = p2p.Send(rw, eth.ReceiptsMsg, &p)
		}
		if err != nil {
			return err
		}
		if mutation == "wrong_receipt_id" || mutation == "no_progress" {
			return nil
		}
	}
	return nil
}
func TestReceiptFetch69And70PartialConsensusBytes(t *testing.T) {
	for _, version := range []uint{eth.ETH69, eth.ETH70, eth.ETH72} {
		a, b := p2p.MsgPipe()
		ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		done := make(chan error, 1)
		go func() { done <- sendReceiptFixture(b, version, "", true) }()
		got, err := fetchReceiptBlock(ctx, a, version, 123)
		if err != nil {
			a.Close()
			b.Close()
			cancel()
			t.Fatal(err)
		}
		if err = <-done; err != nil {
			t.Fatal(err)
		}
		want, header := receiptFixture()
		if got.Number != 123 || got.ReceiptsRoot != header.ReceiptHash || crypto.Keccak256Hash(got.Header) != got.Hash || len(got.Receipts) != 2 {
			t.Fatal("incomplete receipt block")
		}
		for i, r := range want {
			raw, e := r.MarshalBinary()
			if e != nil || !bytes.Equal(raw, got.Receipts[i]) {
				t.Fatal("noncanonical receipt reconstruction", e)
			}
		}
		a.Close()
		b.Close()
		cancel()
	}
}
func TestReceiptFetchBindingsAndMalformedPages(t *testing.T) {
	for _, mutation := range []string{"wrong_header_id", "wrong_receipt_id", "wrong_number", "wrong_root", "no_progress", "unknown_type", "gas_backwards", "gas_final"} {
		t.Run(mutation, func(t *testing.T) {
			a, b := p2p.MsgPipe()
			defer a.Close()
			defer b.Close()
			ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
			defer cancel()
			done := make(chan error, 1)
			go func() { done <- sendReceiptFixture(b, eth.ETH72, mutation, false) }()
			got, err := fetchReceiptBlock(ctx, a, eth.ETH72, 123)
			if err == nil || len(got.Header) != 0 || len(got.Receipts) != 0 {
				t.Fatal("invalid response emitted partial availability")
			}
			a.Close()
			b.Close()
			if err = <-done; err != nil && !errors.Is(err, io.EOF) {
				t.Fatal(err)
			}
		})
	}
}
func TestReceiptFetchCanonicalEmptyBlock(t *testing.T) {
	a, b := p2p.MsgPipe()
	defer a.Close()
	defer b.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- sendReceiptFixture(b, eth.ETH72, "empty_block", false) }()
	got, err := fetchReceiptBlock(ctx, a, eth.ETH72, 123)
	if err != nil || len(got.Receipts) != 0 || got.ReceiptsRoot != types.EmptyReceiptsHash {
		t.Fatal("empty canonical block refused", err)
	}
	if err = <-done; err != nil {
		t.Fatal(err)
	}
}

type receiptBudgetRW struct {
	message p2p.Msg
	reads   int
}

func (r *receiptBudgetRW) ReadMsg() (p2p.Msg, error) { r.reads++; return r.message, nil }
func (r *receiptBudgetRW) WriteMsg(p2p.Msg) error    { return nil }
func TestReceiptFetchMessageAndFramingBounds(t *testing.T) {
	rw := &receiptBudgetRW{message: p2p.Msg{Size: maxReceiptFetchMessage + 1}}
	reader := receiptFetchReader{ctx: context.Background(), rw: rw}
	if _, err := reader.next(); err == nil {
		t.Fatal("oversize message accepted")
	}
	reader = receiptFetchReader{ctx: context.Background(), rw: rw, messages: 256}
	if _, err := reader.next(); err == nil {
		t.Fatal("message count accepted")
	}
	raw, _ := rlp.EncodeToBytes(&eth.GetReceiptsPacket69{RequestId: 1})
	raw = append(raw, 0xc0)
	if receiptDecode(p2p.Msg{Size: uint32(len(raw)), Payload: bytes.NewReader(raw)}, new(eth.GetReceiptsPacket69)) == nil {
		t.Fatal("trailing RLP accepted")
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	reader = receiptFetchReader{ctx: ctx, rw: rw}
	if _, err := reader.next(); !errors.Is(err, context.Canceled) {
		t.Fatal("cancel ignored")
	}
	if _, err := fetchReceiptBlock(context.Background(), rw, 68, 123); err == nil {
		t.Fatal("unsupported protocol accepted")
	}
}

func TestReceiptFetchCountsAndPageLimit(t *testing.T) {
	for _, mode := range []string{"missing_block", "two_blocks", "too_many_receipts", "page_limit"} {
		t.Run(mode, func(t *testing.T) {
			a, b := p2p.MsgPipe()
			defer a.Close()
			defer b.Close()
			ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
			defer cancel()
			done := make(chan error, 1)
			go func() {
				receipts, header := receiptFixture()
				m, err := b.ReadMsg()
				if err != nil {
					done <- err
					return
				}
				var h eth.GetBlockHeadersPacket
				if err = m.Decode(&h); err != nil {
					done <- err
					return
				}
				raw, _ := rlp.EncodeToBytes(&header)
				if err = p2p.Send(b, eth.BlockHeadersMsg, &eth.BlockHeadersRLPPacket{RequestId: h.RequestId, BlockHeadersRLPResponse: []rlp.RawValue{raw}}); err != nil {
					done <- err
					return
				}
				pages := 1
				if mode == "page_limit" {
					pages = maxReceiptPages
				}
				ids := map[uint64]bool{}
				for page := 0; page < pages; page++ {
					m, err = b.ReadMsg()
					if err != nil {
						done <- err
						return
					}
					var q eth.GetReceiptsPacket70
					if m.Code != eth.GetReceiptsMsg || m.Decode(&q) != nil || q.FirstBlockReceiptIndex != uint64(page) || ids[q.RequestId] {
						done <- errors.New("partial query index/id did not progress")
						return
					}
					ids[q.RequestId] = true
					p := eth.ReceiptsPacket70{RequestId: q.RequestId}
					items := receipts[:1]
					switch mode {
					case "missing_block":
					case "two_blocks":
						p.List.Append(eth.NewReceiptList(items))
						p.List.Append(eth.NewReceiptList(items))
					case "too_many_receipts":
						all := make([]*types.Receipt, maxFetchedReceipts+1)
						for i := range all {
							all[i] = receipts[0]
						}
						p.List.Append(eth.NewReceiptList(all))
					case "page_limit":
						p.LastBlockIncomplete = true
						p.List.Append(eth.NewReceiptList(items))
					}
					if err = p2p.Send(b, eth.ReceiptsMsg, &p); err != nil {
						done <- err
						return
					}
				}
				done <- nil
			}()
			got, err := fetchReceiptBlock(ctx, a, eth.ETH72, 123)
			if err == nil || len(got.Header) != 0 || len(got.Receipts) != 0 {
				t.Fatal("count/page budget accepted or returned partial block")
			}
			a.Close()
			b.Close()
			if e := <-done; e != nil && !errors.Is(e, io.EOF) {
				t.Fatal(e)
			}
		})
	}
}

func TestReceiptFetchHeaderCountAndTruncatedMessage(t *testing.T) {
	for _, count := range []int{0, 2} {
		a, b := p2p.MsgPipe()
		ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		done := make(chan error, 1)
		go func() {
			m, e := b.ReadMsg()
			if e != nil {
				done <- e
				return
			}
			var q eth.GetBlockHeadersPacket
			if e = m.Decode(&q); e != nil {
				done <- e
				return
			}
			_, h := receiptFixture()
			raw, _ := rlp.EncodeToBytes(&h)
			headers := []rlp.RawValue{}
			for i := 0; i < count; i++ {
				headers = append(headers, raw)
			}
			done <- p2p.Send(b, eth.BlockHeadersMsg, &eth.BlockHeadersRLPPacket{RequestId: q.RequestId, BlockHeadersRLPResponse: headers})
		}()
		if block, e := fetchReceiptBlock(ctx, a, eth.ETH72, 123); e == nil || len(block.Header) != 0 {
			t.Fatal("wrong header count accepted")
		}
		a.Close()
		b.Close()
		cancel()
		if e := <-done; e != nil {
			t.Fatal(e)
		}
	}
	rw := &receiptBudgetRW{message: p2p.Msg{Size: 10, Payload: bytes.NewReader([]byte{1, 2})}}
	reader := receiptFetchReader{ctx: context.Background(), rw: rw}
	if _, e := reader.next(); e == nil {
		t.Fatal("truncated frame accepted")
	}
}
