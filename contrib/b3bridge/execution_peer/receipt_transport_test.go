// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license.
package main

import (
	"bytes"
	eth "github.com/ethereum/go-ethereum/eth/protocols/eth"
	"github.com/ethereum/go-ethereum/p2p"
	"testing"
)

type receiptBudgetFixture struct {
	message p2p.Msg
	writes  int
}

func (f *receiptBudgetFixture) ReadMsg() (p2p.Msg, error) { return f.message, nil }
func (f *receiptBudgetFixture) WriteMsg(p2p.Msg) error    { f.writes++; return nil }

func TestReceiptTransportRefusesTransactionsAndBudgets(t *testing.T) {
	inner := &receiptBudgetFixture{}
	rw := &boundedReceiptRW{MsgReadWriter: inner}
	for _, code := range []uint64{eth.TransactionsMsg, eth.PooledTransactionsMsg, eth.GetPooledTransactionsMsg} {
		if rw.WriteMsg(p2p.Msg{Code: code}) == nil {
			t.Fatal("transaction message permitted")
		}
	}
	if inner.writes != 0 {
		t.Fatal("denied messages escaped")
	}
	for _, code := range []uint64{eth.StatusMsg, eth.GetBlockHeadersMsg, eth.GetReceiptsMsg, eth.BlockHeadersMsg, eth.BlockBodiesMsg} {
		if err := rw.WriteMsg(p2p.Msg{Code: code}); err != nil {
			t.Fatal(err)
		}
	}
	inner.message = p2p.Msg{Size: 2*1024*1024 + 1, Payload: bytes.NewReader(nil)}
	if _, err := rw.ReadMsg(); err == nil {
		t.Fatal("oversized receipt message accepted")
	}
	inner.message = p2p.Msg{Size: 1, Payload: bytes.NewReader([]byte{0})}
	rw = &boundedReceiptRW{MsgReadWriter: inner, bytes: maxWitnessBytes}
	if _, err := rw.ReadMsg(); err == nil {
		t.Fatal("aggregate receipt budget accepted")
	}
	rw = &boundedReceiptRW{MsgReadWriter: inner, messages: 256}
	if _, err := rw.ReadMsg(); err == nil {
		t.Fatal("receipt message count accepted")
	}
}

func TestReceiptModeRequiresCanonicalNumberAndNoAuthority(t *testing.T) {
	for _, text := range []string{"", "0", "00", "01", "+1", "-1", " 1", "1 ", "18446744073709551616"} {
		if _, err := parseReceiptNumber(text); err == nil {
			t.Fatalf("bad number accepted: %q", text)
		}
	}
	if n, err := parseReceiptNumber("11995637"); err != nil || n != 11995637 {
		t.Fatal("canonical number refused")
	}
	for _, hash := range []string{" ", "0xabc"} {
		if !receiptModeConflict(hash, false, "") {
			t.Fatal("ambiguous receipt mode accepted")
		}
	}
	r := receiptResult()
	if r.Authority || r.Mainnet || !r.NativeRequired || !r.AvailabilityOnly || r.Passed {
		t.Fatal("receipt bytes conferred source authority")
	}
}
