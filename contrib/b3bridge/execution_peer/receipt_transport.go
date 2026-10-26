// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license.
package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"strconv"
	"sync/atomic"
	"time"

	eth "github.com/ethereum/go-ethereum/eth/protocols/eth"
	"github.com/ethereum/go-ethereum/p2p"
)

// Header/receipt bytes are untrusted availability. Even a matching receipt
// trie root cannot authenticate a peer-provided block header or finality.
type boundedReceiptRW struct {
	p2p.MsgReadWriter
	bytes    uint64
	messages uint64
}

func (rw *boundedReceiptRW) ReadMsg() (p2p.Msg, error) {
	m, err := rw.MsgReadWriter.ReadMsg()
	if err != nil {
		return m, err
	}
	if m.Size > 2*1024*1024 || rw.bytes+uint64(m.Size) > maxWitnessBytes || rw.messages >= 256 {
		return p2p.Msg{}, errors.New("receipt peer byte/message budget exceeded")
	}
	rw.bytes += uint64(m.Size)
	rw.messages++
	return m, nil
}
func (rw *boundedReceiptRW) WriteMsg(m p2p.Msg) error {
	if unavailableETHResponse(m.Code) {
		if err := validateUnavailableETHReply(&m); err != nil {
			return err
		}
		return rw.MsgReadWriter.WriteMsg(m)
	}
	switch m.Code {
	case eth.StatusMsg, eth.GetBlockHeadersMsg, eth.GetReceiptsMsg,
		eth.BlockHeadersMsg, eth.BlockBodiesMsg, eth.ReceiptsMsg:
		return rw.MsgReadWriter.WriteMsg(m)
	default:
		return errors.New("read-only receipt transport refuses outbound message")
	}
}

func receiptResult() result {
	out := baseResult()
	out.Format = "b3-ethereum-peer-receipts/1"
	return out
}

func receiptPeerTask(number uint64) peerTask {
	return func(ctx context.Context, p *p2p.Peer, rw p2p.MsgReadWriter, version uint,
		chain genesisChain, output chan<- result, stats *counters) (returned error) {
		phase := "receipt_eth_handshake"
		defer func() {
			if returned != nil {
				counter, _ := stats.Stages.LoadOrStore(phase, new(atomic.Uint64))
				counter.(*atomic.Uint64).Add(1)
			}
		}()
		stats.Connected.Add(1)
		bounded := &boundedReceiptRW{MsgReadWriter: rw}
		peer := eth.NewPeer(version, p, bounded, nil, nil, chain.Config())
		defer peer.Close()
		timer := time.AfterFunc(90*time.Second, func() { p.Disconnect(p2p.DiscReadTimeout) })
		defer timer.Stop()
		if err := peer.Handshake(networkID, chain, eth.BlockRangeUpdatePacket{LatestBlockHash: chain.block.Hash()}); err != nil {
			stats.Refusals.Add(1)
			return err
		}
		stats.Handshakes.Add(1)
		phase = "receipt_availability"
		block, err := fetchReceiptBlock(ctx, bounded, version, number)
		if err != nil {
			return err
		}
		if err = ctx.Err(); err != nil {
			return err
		}
		value := receiptResult()
		value.Passed = true
		value.BlockNumber = block.Number
		value.BlockHash = block.Hash.Hex()
		value.ReceiptsRoot = block.ReceiptsRoot.Hex()
		value.ReceiptCommitmentsMatch = true
		value.Header = "0x" + hex.EncodeToString(block.Header)
		value.Protocol = version
		digest := sha256.Sum256(block.Header)
		value.HeaderSHA256 = hex.EncodeToString(digest[:])
		id := p.ID()
		digest = sha256.Sum256(id[:])
		value.PeerSHA256 = hex.EncodeToString(digest[:])
		value.Receipts = make([]string, len(block.Receipts))
		for i, raw := range block.Receipts {
			value.Receipts[i] = "0x" + hex.EncodeToString(raw)
		}
		encoded, err := json.Marshal(value)
		if err != nil || len(encoded) > maxWitnessBytes {
			return errors.New("receipt output byte bound exceeded")
		}
		select {
		case output <- value:
			return nil
		case <-ctx.Done():
			return ctx.Err()
		}
	}
}

func parseReceiptNumber(text string) (uint64, error) {
	n, err := strconv.ParseUint(text, 10, 64)
	if err != nil || n == 0 || strconv.FormatUint(n, 10) != text {
		return 0, errors.New("canonical positive receipt block number required")
	}
	return n, nil
}
func runReceiptBlock(text string, seconds, maxPeers int, fixture string) int {
	out := receiptResult()
	number, err := parseReceiptNumber(text)
	if err != nil || seconds < 1 || seconds > 300 || maxPeers < 1 || maxPeers > 8 {
		out.Failure = "invalid bounded receipt request"
		json.NewEncoder(os.Stdout).Encode(out)
		return 2
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Duration(seconds)*time.Second)
	defer cancel()
	started := time.Now()
	out, err = collect(ctx, [32]byte{}, false, maxPeers, fixture, receiptPeerTask(number))
	// A timeout returns the generic header base; retain this mode's scope.
	out.Format = "b3-ethereum-peer-receipts/1"
	out.Seconds = time.Since(started).Seconds()
	if err != nil {
		out.Failure = "receipt availability refused or timed out"
	}
	raw, encodeErr := encodeAvailabilityJSON(out)
	if encodeErr != nil {
		out = receiptResult()
		out.Failure = "final receipt output byte bound exceeded"
		raw, _ = encodeAvailabilityJSON(out)
	}
	if _, writeErr := os.Stdout.Write(raw); writeErr != nil {
		return 1
	}
	if err != nil || !out.Passed {
		return 1
	}
	return 0
}

// Keep data-only mode selection explicit; no URL, chain override or signer.
func receiptModeConflict(hash string, tip bool, witnessFile string) bool {
	return hash != "" || tip || witnessFile != ""
}
