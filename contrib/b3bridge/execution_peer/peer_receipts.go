// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license.
package main

import (
	"bytes"
	"context"
	"errors"
	"io"

	"github.com/ethereum/go-ethereum/common"
	"github.com/ethereum/go-ethereum/core/types"
	"github.com/ethereum/go-ethereum/crypto"
	eth "github.com/ethereum/go-ethereum/eth/protocols/eth"
	"github.com/ethereum/go-ethereum/p2p"
	"github.com/ethereum/go-ethereum/rlp"
	"github.com/ethereum/go-ethereum/trie"
)

// ReceiptBlock contains availability bytes only. Number-based header retrieval
// and receipt-root consistency do not establish finalized Ethereum ancestry.
// Native latestConfirmed state and the assertion preimage hash remain the
// authority for the caller's eventual Arbitrum settlement check.
type ReceiptBlock struct {
	Header             []byte
	Receipts           [][]byte
	Number             uint64
	Hash, ReceiptsRoot common.Hash
}

const maxFetchedReceipts = 8192
const maxReceiptPages = 128
const maxReceiptFetchBytes = 32 * 1024 * 1024
const maxReceiptFetchMessage = 2 * 1024 * 1024

type receiptFetchReader struct {
	ctx      context.Context
	rw       p2p.MsgReadWriter
	messages int
	bytes    uint64
}

func (r *receiptFetchReader) next() (p2p.Msg, error) {
	if err := r.ctx.Err(); err != nil {
		return p2p.Msg{}, err
	}
	if r.messages >= 256 {
		return p2p.Msg{}, errors.New("receipt message count exceeded")
	}
	m, err := r.rw.ReadMsg()
	if err != nil {
		return p2p.Msg{}, err
	}
	if m.Size > maxReceiptFetchMessage || r.bytes+uint64(m.Size) > maxReceiptFetchBytes {
		return p2p.Msg{}, errors.New("receipt message bytes exceeded")
	}
	r.messages++
	r.bytes += uint64(m.Size)
	raw, err := io.ReadAll(io.LimitReader(m.Payload, int64(m.Size)+1))
	if err != nil || len(raw) != int(m.Size) {
		return p2p.Msg{}, errors.New("incomplete receipt transport message")
	}
	m.Payload = bytes.NewReader(raw)
	return m, nil
}
func receiptDecode(m p2p.Msg, out any) error {
	raw, err := io.ReadAll(io.LimitReader(m.Payload, int64(m.Size)+1))
	if err != nil || len(raw) != int(m.Size) {
		return errors.New("incomplete receipt packet")
	}
	return rlp.DecodeBytes(raw, out)
}

// Answer a bounded inbound query with an honest unavailable response while
// waiting for our own response. The owning transport restricts outbound types.
func replyUnavailableReceiptPeer(m p2p.Msg, rw p2p.MsgReadWriter, version uint) (bool, error) {
	switch m.Code {
	case eth.GetBlockHeadersMsg:
		var q eth.GetBlockHeadersPacket
		if err := receiptDecode(m, &q); err != nil {
			return true, err
		}
		return true, p2p.Send(rw, eth.BlockHeadersMsg, &eth.BlockHeadersRLPPacket{RequestId: q.RequestId})
	case eth.GetBlockBodiesMsg:
		var q eth.GetBlockBodiesPacket
		if err := receiptDecode(m, &q); err != nil {
			return true, err
		}
		return true, p2p.Send(rw, eth.BlockBodiesMsg, &eth.BlockBodiesRLPPacket{RequestId: q.RequestId})
	case eth.GetReceiptsMsg, eth.GetBlockAccessListsMsg, eth.GetCellsMsg:
		return replyUnavailableETHData(m, rw, version)
	case eth.NewPooledTransactionHashesMsg, eth.TransactionsMsg, eth.BlockRangeUpdateMsg:
		return true, m.Discard()
	}
	return false, nil
}
func (r *receiptFetchReader) expect(code uint64, version uint) (p2p.Msg, error) {
	for {
		m, err := r.next()
		if err != nil {
			return p2p.Msg{}, err
		}
		if m.Code == code {
			return m, nil
		}
		handled, err := replyUnavailableReceiptPeer(m, r.rw, version)
		if err != nil {
			return p2p.Msg{}, err
		}
		if !handled {
			return p2p.Msg{}, errors.New("unexpected receipt peer message")
		}
	}
}

// The owner supplies the same-peer Sepolia handshake, global deadline and
// physical-peer disconnect timer; this helper opens no network connection.
func fetchReceiptBlock(ctx context.Context, rw p2p.MsgReadWriter, version uint, number uint64) (ReceiptBlock, error) {
	empty := ReceiptBlock{}
	if version < eth.ETH69 || version > eth.ETH72 {
		return empty, errors.New("unsupported receipt protocol")
	}
	reader := receiptFetchReader{ctx: ctx, rw: rw}
	id, err := witnessRequestID()
	if err != nil {
		return empty, err
	}
	q := eth.GetBlockHeadersPacket{RequestId: id, GetBlockHeadersRequest: &eth.GetBlockHeadersRequest{Origin: eth.HashOrNumber{Number: number}, Amount: 1}}
	if err = p2p.Send(rw, eth.GetBlockHeadersMsg, &q); err != nil {
		return empty, err
	}
	m, err := reader.expect(eth.BlockHeadersMsg, version)
	if err != nil {
		return empty, err
	}
	var hp eth.BlockHeadersRLPPacket
	if err = receiptDecode(m, &hp); err != nil {
		return empty, err
	}
	if hp.RequestId != id || len(hp.BlockHeadersRLPResponse) != 1 {
		return empty, errors.New("receipt header request or count mismatch")
	}
	raw := hp.BlockHeadersRLPResponse[0]
	if len(raw) == 0 || len(raw) > maxHeaderBytes {
		return empty, errors.New("receipt header byte limit")
	}
	var header types.Header
	if err = rlp.DecodeBytes(raw, &header); err != nil || header.Number == nil || header.Number.Sign() < 0 || header.Number.BitLen() > 64 || header.Number.Uint64() != number || header.ReceiptHash == (common.Hash{}) || header.GasUsed > header.GasLimit {
		return empty, errors.New("malformed receipt header")
	}
	block := ReceiptBlock{Header: append([]byte(nil), raw...), Number: number, Hash: crypto.Keccak256Hash(raw), ReceiptsRoot: header.ReceiptHash}
	combined := new(eth.ReceiptList)
	complete := false
	for page := 0; page < maxReceiptPages; page++ {
		if err = ctx.Err(); err != nil {
			return empty, err
		}
		id, err = witnessRequestID()
		if err != nil {
			return empty, err
		}
		offset := combined.Derivable().Len()
		if version == eth.ETH69 {
			if err = p2p.Send(rw, eth.GetReceiptsMsg, &eth.GetReceiptsPacket69{RequestId: id, GetReceiptsRequest: eth.GetReceiptsRequest{block.Hash}}); err != nil {
				return empty, err
			}
		} else if err = p2p.Send(rw, eth.GetReceiptsMsg, &eth.GetReceiptsPacket70{RequestId: id, FirstBlockReceiptIndex: uint64(offset), GetReceiptsRequest: eth.GetReceiptsRequest{block.Hash}}); err != nil {
			return empty, err
		}
		m, err = reader.expect(eth.ReceiptsMsg, version)
		if err != nil {
			return empty, err
		}
		var lists []*eth.ReceiptList
		incomplete := false
		if version == eth.ETH69 {
			var p eth.ReceiptsPacket69
			if err = receiptDecode(m, &p); err != nil {
				return empty, err
			}
			if p.RequestId != id || p.List.Len() != 1 {
				return empty, errors.New("receipt request or block count mismatch")
			}
			lists, err = p.List.Items()
		} else {
			var p eth.ReceiptsPacket70
			if err = receiptDecode(m, &p); err != nil {
				return empty, err
			}
			if p.RequestId != id || p.List.Len() != 1 {
				return empty, errors.New("receipt request or block count mismatch")
			}
			incomplete = p.LastBlockIncomplete
			lists, err = p.List.Items()
		}
		if err != nil || len(lists) != 1 || lists[0] == nil {
			return empty, errors.New("malformed receipt list")
		}
		count := lists[0].Derivable().Len()
		if count > maxFetchedReceipts-offset {
			return empty, errors.New("receipt count exceeded")
		}
		if count == 0 && (incomplete || offset != 0 || header.ReceiptHash != types.EmptyReceiptsHash) {
			return empty, errors.New("receipt page made no progress")
		}
		combined.Append(lists[0])
		if !incomplete {
			complete = true
			break
		}
	}
	if !complete {
		return empty, errors.New("receipt page count exceeded")
	}
	derivable := combined.Derivable()
	if types.DeriveSha(derivable, trie.NewStackTrie(nil)) != header.ReceiptHash {
		return empty, errors.New("receipt trie root mismatch")
	}
	total := len(block.Header)
	var gas uint64
	for i := 0; i < derivable.Len(); i++ {
		var encoded bytes.Buffer
		derivable.EncodeIndex(i, &encoded)
		b := encoded.Bytes()
		if len(b) == 0 || len(b) > maxReceiptFetchBytes-total {
			return empty, errors.New("raw receipt byte limit")
		}
		var receipt types.Receipt
		if err = receipt.UnmarshalBinary(b); err != nil || receipt.CumulativeGasUsed < gas || receipt.CumulativeGasUsed > header.GasUsed {
			return empty, errors.New("malformed consensus receipt")
		}
		gas = receipt.CumulativeGasUsed
		total += len(b)
		block.Receipts = append(block.Receipts, append([]byte(nil), b...))
	}
	if gas != header.GasUsed {
		return empty, errors.New("receipt cumulative gas mismatch")
	}
	return block, nil
}
