// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license.

// Offline JSON-to-consensus-byte serializer using the exact maintained Nitro
// Geth fork. It has no RPC client, key, network service or source authority.
package main

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"

	"github.com/ethereum/go-ethereum/common"
	"github.com/ethereum/go-ethereum/core/types"
	"github.com/ethereum/go-ethereum/rlp"
	"github.com/ethereum/go-ethereum/trie"
)

const maxInput = 32 * 1024 * 1024
const maxRecords = 8192

type request struct {
	Mode         string            `json:"mode"`
	ExpectedHash common.Hash       `json:"expected_hash"`
	Block        json.RawMessage   `json:"block"`
	Receipts     []json.RawMessage `json:"receipts"`
}

type reply struct {
	Format           string      `json:"format"`
	Hash             common.Hash `json:"hash"`
	Number           uint64      `json:"number"`
	Parent           common.Hash `json:"parent_hash"`
	Header           string      `json:"header"`
	Transactions     []string    `json:"transactions"`
	Receipts         []string    `json:"receipts"`
	CommitmentsMatch bool        `json:"commitments_match"`
	SourceAuthority  bool        `json:"source_authority"`
	MainnetEnabled   bool        `json:"mainnet_enabled"`
}

func encoded(raw []byte) string { return "0x" + hex.EncodeToString(raw) }

func convert(in request) (reply, error) {
	var out reply
	if in.ExpectedHash == (common.Hash{}) || (in.Mode != "header" && in.Mode != "block") {
		return out, errors.New("exact requested hash and mode required")
	}
	var header types.Header
	if err := json.Unmarshal(in.Block, &header); err != nil {
		return out, errors.New("maintained header decoder refused JSON")
	}
	if header.Number == nil || !header.Number.IsUint64() || header.Hash() != in.ExpectedHash {
		return out, errors.New("header does not open requested hash")
	}
	var metadata struct {
		Hash         common.Hash       `json:"hash"`
		Transactions []json.RawMessage `json:"transactions"`
	}
	if err := json.Unmarshal(in.Block, &metadata); err != nil || metadata.Hash != in.ExpectedHash {
		return out, errors.New("block metadata hash differs")
	}
	rawHeader, err := rlp.EncodeToBytes(&header)
	if err != nil || len(rawHeader) == 0 || len(rawHeader) > 4096 {
		return out, errors.New("bounded header encoding refused")
	}
	out = reply{Format: "b3-maintained-nitro-json-codec/1", Hash: header.Hash(), Number: header.Number.Uint64(), Parent: header.ParentHash, Header: encoded(rawHeader), Transactions: []string{}, Receipts: []string{}}
	if in.Mode == "header" {
		if len(in.Receipts) != 0 {
			return reply{}, errors.New("header-only request has receipts")
		}
		return out, nil
	}
	if len(metadata.Transactions) > maxRecords || len(metadata.Transactions) != len(in.Receipts) {
		return reply{}, errors.New("complete transaction/receipt count differs")
	}
	txs := make(types.Transactions, len(metadata.Transactions))
	receipts := make(types.Receipts, len(in.Receipts))
	var previousGas uint64
	for i, raw := range metadata.Transactions {
		if len(raw) == 0 || raw[0] != '{' {
			return reply{}, errors.New("full transaction objects required")
		}
		var tx types.Transaction
		if err := json.Unmarshal(raw, &tx); err != nil {
			return reply{}, fmt.Errorf("maintained transaction decoder refused index %d", i)
		}
		var txMetadata struct {
			Hash      common.Hash `json:"hash"`
			BlockHash common.Hash `json:"blockHash"`
		}
		if err := json.Unmarshal(raw, &txMetadata); err != nil || tx.Hash() != txMetadata.Hash || txMetadata.BlockHash != header.Hash() {
			return reply{}, errors.New("transaction hash or containing block differs")
		}
		var receipt types.Receipt
		if err := json.Unmarshal(in.Receipts[i], &receipt); err != nil {
			return reply{}, fmt.Errorf("maintained receipt decoder refused index %d", i)
		}
		if receipt.BlockHash != header.Hash() || receipt.BlockNumber == nil || receipt.BlockNumber.Cmp(header.Number) != 0 || receipt.TransactionIndex != uint(i) || receipt.TxHash != tx.Hash() || receipt.Type != tx.Type() {
			return reply{}, errors.New("receipt coordinates or type differ")
		}
		if receipt.CumulativeGasUsed < previousGas || receipt.CumulativeGasUsed > header.GasUsed {
			return reply{}, errors.New("receipt cumulative gas differs")
		}
		previousGas = receipt.CumulativeGasUsed
		txBytes, err := tx.MarshalBinary()
		if err != nil || len(txBytes) == 0 || len(txBytes) > 1024*1024 {
			return reply{}, errors.New("bounded transaction serialization refused")
		}
		receiptBytes, err := receipt.MarshalBinary()
		if err != nil || len(receiptBytes) == 0 || len(receiptBytes) > 1024*1024 {
			return reply{}, errors.New("bounded receipt serialization refused")
		}
		txs[i], receipts[i] = &tx, &receipt
		out.Transactions = append(out.Transactions, encoded(txBytes))
		out.Receipts = append(out.Receipts, encoded(receiptBytes))
	}
	if previousGas != header.GasUsed || types.DeriveSha(txs, trie.NewStackTrie(nil)) != header.TxHash || types.DeriveSha(receipts, trie.NewStackTrie(nil)) != header.ReceiptHash {
		return reply{}, errors.New("complete transaction/receipt root or final gas differs")
	}
	out.CommitmentsMatch = true
	return out, nil
}

func main() {
	raw, err := io.ReadAll(io.LimitReader(os.Stdin, maxInput+1))
	if err != nil || len(raw) > maxInput {
		fmt.Fprintln(os.Stderr, "bounded JSON input required")
		os.Exit(2)
	}
	var in request
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&in); err != nil {
		fmt.Fprintln(os.Stderr, "request JSON refused")
		os.Exit(2)
	}
	if err := decoder.Decode(new(any)); err != io.EOF {
		fmt.Fprintln(os.Stderr, "trailing JSON refused")
		os.Exit(2)
	}
	out, err := convert(in)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	if err := json.NewEncoder(os.Stdout).Encode(out); err != nil {
		os.Exit(1)
	}
}
