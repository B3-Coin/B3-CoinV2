// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license.
package main

import (
	"bytes"
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"github.com/ethereum/go-ethereum/core"
	eth "github.com/ethereum/go-ethereum/eth/protocols/eth"
	"github.com/ethereum/go-ethereum/log"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/ethereum/go-ethereum/common"
	"github.com/ethereum/go-ethereum/core/rawdb"
	"github.com/ethereum/go-ethereum/core/types"
	"github.com/ethereum/go-ethereum/crypto"
	snap "github.com/ethereum/go-ethereum/eth/protocols/snap"
	"github.com/ethereum/go-ethereum/ethdb/memorydb"
	"github.com/ethereum/go-ethereum/p2p"
	"github.com/ethereum/go-ethereum/p2p/enode"
	"github.com/ethereum/go-ethereum/rlp"
	"github.com/ethereum/go-ethereum/trie"
	"github.com/ethereum/go-ethereum/triedb"
)

func witnessFixture(t *testing.T) (witnessRequest, common.Hash, common.Hash, common.Hash, types.StateAccount, [][]byte, [][]byte) {
	t.Helper()
	address := common.HexToAddress("0x1234")
	slot := common.HexToHash("0x2a")
	accountKey := crypto.Keccak256Hash(address[:])
	storageKey := crypto.Keccak256Hash(slot[:])
	db := triedb.NewDatabase(rawdb.NewMemoryDatabase(), nil)
	storage := trie.NewEmpty(db)
	storage.MustUpdate(storageKey[:], []byte{0x07})
	account := *types.NewEmptyStateAccount()
	account.Root = storage.Hash()
	account.CodeHash = crypto.Keccak256([]byte("reviewed-contract"))
	accounts := trie.NewEmpty(db)
	full, err := rlp.EncodeToBytes(&account)
	if err != nil {
		t.Fatal(err)
	}
	accounts.MustUpdate(accountKey[:], full)
	nodes := func(tree *trie.Trie, key common.Hash) [][]byte {
		proof := memorydb.New()
		if err := tree.Prove(key[:], proof); err != nil {
			t.Fatal(err)
		}
		it := proof.NewIterator(nil, nil)
		defer it.Release()
		var out [][]byte
		for it.Next() {
			out = append(out, bytes.Clone(it.Value()))
		}
		return out
	}
	req := witnessRequest{StateRoot: accounts.Hash().Hex(), Accounts: []witnessAccountRequest{{Address: strings.ToLower(address.Hex()), StorageKeys: []string{slot.Hex()}}}}
	return req, accounts.Hash(), accountKey, storageKey, account, nodes(accounts, accountKey), nodes(storage, storageKey)
}
func TestWitnessRequestBoundaries(t *testing.T) {
	req, _, _, _, _, _, _ := witnessFixture(t)
	raw, _ := json.Marshal(req)
	if _, err := decodeWitnessRequest(raw); err != nil {
		t.Fatal(err)
	}
	bad := [][]byte{append(raw, raw...), []byte(strings.Replace(string(raw), `"state_root":`, `"state_root":"0x`+strings.Repeat("11", 32)+`","state_root":`, 1)), []byte(strings.Replace(string(raw), `"accounts":`, `"verified":true,"accounts":`, 1)), bytes.Repeat([]byte{' '}, 65537)}
	for _, b := range bad {
		if _, err := decodeWitnessRequest(b); err == nil {
			t.Fatalf("accepted malformed request %q", b[:min(len(b), 80)])
		}
	}
	req.Accounts = append(req.Accounts, req.Accounts[0])
	raw, _ = json.Marshal(req)
	if _, err := decodeWitnessRequest(raw); err == nil {
		t.Fatal("duplicate account accepted")
	}
	req.Accounts = req.Accounts[:1]
	req.Accounts[0].StorageKeys = nil
	for i := 0; i < 66; i++ {
		req.Accounts[0].StorageKeys = append(req.Accounts[0].StorageKeys, fmt.Sprintf("0x%064x", i))
	}
	raw, _ = json.Marshal(req)
	if _, err := decodeWitnessRequest(raw); err == nil {
		t.Fatal("per-account slot bound exceeded")
	}
	req.Accounts = nil
	for i := 1; i <= 17; i++ {
		req.Accounts = append(req.Accounts, witnessAccountRequest{Address: fmt.Sprintf("0x%040x", i)})
	}
	raw, _ = json.Marshal(req)
	if _, err := decodeWitnessRequest(raw); err == nil {
		t.Fatal("account count bound exceeded")
	}
	req.Accounts = req.Accounts[:2]
	for i := range req.Accounts {
		for j := 0; j < 65; j++ {
			req.Accounts[i].StorageKeys = append(req.Accounts[i].StorageKeys, fmt.Sprintf("0x%064x", j))
		}
	}
	raw, _ = json.Marshal(req)
	if _, err := decodeWitnessRequest(raw); err == nil {
		t.Fatal("total slot bound exceeded")
	}
	req.Accounts[1].StorageKeys = req.Accounts[1].StorageKeys[:63]
	raw, _ = json.Marshal(req)
	if _, err := decodeWitnessRequest(raw); err != nil {
		t.Fatal("128-slot bound rejected", err)
	}
}
func TestOrderedWitnessInclusionAbsenceAndTamper(t *testing.T) {
	_, root, key, slot, account, proof, storage := witnessFixture(t)
	nodes, value, err := orderedWitness(root, key, proof)
	if err != nil || len(nodes) == 0 || len(value) == 0 {
		t.Fatal("valid account proof refused", err)
	}
	if _, _, err = orderedWitness(common.HexToHash("0xdead"), key, proof); err == nil {
		t.Fatal("wrong root accepted")
	}
	if _, _, err = orderedWitness(root, key, append(proof, proof[0])); err == nil {
		t.Fatal("duplicate nodes accepted")
	}
	mutated := bytes.Clone(proof[0])
	mutated[len(mutated)-1] ^= 1
	if _, _, err = orderedWitness(root, key, [][]byte{mutated}); err == nil {
		t.Fatal("mutated proof accepted")
	}
	if _, _, err = orderedWitness(root, key, [][]byte{bytes.Repeat([]byte{1}, maxProofNodeBytes+1)}); err == nil {
		t.Fatal("oversize node accepted")
	}
	if _, value, err = orderedWitness(account.Root, common.HexToHash("0xabcd"), storage); err != nil || value != nil {
		t.Fatal("valid absence proof refused", err)
	}
	if _, value, err = orderedWitness(account.Root, slot, storage); err != nil || !bytes.Equal(value, []byte{7}) {
		t.Fatal("valid storage proof refused", err)
	}
	if nodes, value, err = orderedWitness(types.EmptyRootHash, slot, nil); err != nil || value != nil || len(nodes) != 0 {
		t.Fatal("empty trie proof refused", err)
	}
}
func TestSnapAccountStorageResponseBindings(t *testing.T) {
	_, root, key, slot, account, proof, storage := witnessFixture(t)
	packet := snap.AccountRangePacket{ID: 7, Accounts: []*snap.AccountData{{Hash: key, Body: types.SlimAccountRLP(account)}}, Proof: proof}
	if _, got, err := accountRangeWitness(packet, 7, root, key); err != nil || got.Root != account.Root {
		t.Fatal("valid account reply refused", err)
	}
	if _, _, err := accountRangeWitness(packet, 8, root, key); err == nil {
		t.Fatal("wrong request ID accepted")
	}
	packet.Accounts[0].Body = types.SlimAccountRLP(*types.NewEmptyStateAccount())
	if _, _, err := accountRangeWitness(packet, 7, root, key); err == nil {
		t.Fatal("unbound account body accepted")
	}
	sp := snap.StorageRangesPacket{ID: 9, Slots: [][]*snap.StorageData{{{Hash: slot, Body: []byte{7}}}}, Proof: storage}
	if _, err := storageRangeWitness(sp, 9, account.Root, slot); err != nil {
		t.Fatal(err)
	}
	sp.Slots[0][0].Body = []byte{8}
	if _, err := storageRangeWitness(sp, 9, account.Root, slot); err == nil {
		t.Fatal("unbound storage body accepted")
	}
}
func TestActualSnapAccountStorageExchange(t *testing.T) {
	req, root, key, slot, account, proof, storage := witnessFixture(t)
	a, b := p2p.MsgPipe()
	defer a.Close()
	defer b.Close()
	done := make(chan error, 1)
	go func() {
		m, err := b.ReadMsg()
		if err != nil {
			done <- err
			return
		}
		var q snap.GetAccountRangePacket
		if err = m.Decode(&q); err != nil {
			done <- err
			return
		}
		if m.Code != snap.GetAccountRangeMsg || q.Root != root || q.Origin != key || q.Limit != key {
			done <- errors.New("wrong exact account query")
			return
		}
		if err = p2p.Send(b, snap.AccountRangeMsg, &snap.AccountRangePacket{ID: q.ID, Accounts: []*snap.AccountData{{Hash: key, Body: types.SlimAccountRLP(account)}}, Proof: proof}); err != nil {
			done <- err
			return
		}
		m, err = b.ReadMsg()
		if err != nil {
			done <- err
			return
		}
		var s snap.GetStorageRangesPacket
		if err = m.Decode(&s); err != nil {
			done <- err
			return
		}
		if m.Code != snap.GetStorageRangesMsg || s.Root != root || len(s.Accounts) != 1 || s.Accounts[0] != key || !bytes.Equal(s.Origin, slot[:]) || !bytes.Equal(s.Limit, slot[:]) {
			done <- errors.New("wrong exact storage query")
			return
		}
		done <- p2p.Send(b, snap.StorageRangesMsg, &snap.StorageRangesPacket{ID: s.ID, Slots: [][]*snap.StorageData{{{Hash: slot, Body: []byte{7}}}}, Proof: storage})
	}()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	value, err := readWitness(ctx, p2p.NewPeer(enode.ID{9}, "fixture", nil), a, snap.SNAP2, req)
	if err != nil {
		t.Fatal(err)
	}
	if err = <-done; err != nil {
		t.Fatal(err)
	}
	if !value.Passed || value.Authority || value.Mainnet || value.Writes || !value.NativeRequired || len(value.Accounts) != 1 || len(value.Accounts[0].Storage) != 1 {
		t.Fatal("witness escaped data boundary")
	}
	raw, _ := json.Marshal(value.Accounts[0])
	if !bytes.Contains(raw, []byte(`"nodes"`)) || !bytes.Contains(raw, []byte(`"slot"`)) || bytes.Contains(raw, []byte(`"key"`)) {
		t.Fatal("native witness shape mismatch", string(raw))
	}
	for _, n := range value.Accounts[0].AccountProof {
		if _, err := hex.DecodeString(n[2:]); err != nil {
			t.Fatal(err)
		}
	}
}
func TestSnapReadOnlyAndBudgets(t *testing.T) {
	a, b := p2p.MsgPipe()
	defer a.Close()
	defer b.Close()
	rw := &boundedSnapRW{MsgReadWriter: a}
	for _, code := range []uint64{snap.GetByteCodesMsg, snap.GetTrieNodesMsg, snap.GetAccessListsMsg, snap.AccountRangeMsg, 99} {
		if rw.WriteMsg(p2p.Msg{Code: code}) == nil {
			t.Fatal("unneeded outbound protocol admitted")
		}
	}
	go b.WriteMsg(p2p.Msg{Code: snap.AccountRangeMsg, Size: maxSnapMessageBytes + 1, Payload: bytes.NewReader(make([]byte, maxSnapMessageBytes+1))})
	if _, err := rw.ReadMsg(); err == nil {
		t.Fatal("oversize message accepted")
	}
}

func TestOrderedWitnessTrimsRangeBoundaryNodes(t *testing.T) {
	db := triedb.NewDatabase(rawdb.NewMemoryDatabase(), nil)
	tree := trie.NewEmpty(db)
	keys := []common.Hash{}
	for i := 0; i < 32; i++ {
		key := crypto.Keccak256Hash([]byte{byte(i)})
		keys = append(keys, key)
		tree.MustUpdate(key[:], bytes.Repeat([]byte{byte(i + 1)}, 80))
	}
	proof := memorydb.New()
	if err := tree.Prove(keys[0][:], proof); err != nil {
		t.Fatal(err)
	}
	if err := tree.Prove(keys[31][:], proof); err != nil {
		t.Fatal(err)
	}
	it := proof.NewIterator(nil, nil)
	defer it.Release()
	nodes := [][]byte{}
	for it.Next() {
		nodes = append([][]byte{bytes.Clone(it.Value())}, nodes...)
	}
	used, value, err := orderedWitness(tree.Hash(), keys[0], nodes)
	if err != nil || !bytes.Equal(value, bytes.Repeat([]byte{1}, 80)) || len(used) >= len(nodes) {
		t.Fatal("range boundary path was not trimmed", err)
	}
	first, _ := hex.DecodeString(used[0][2:])
	if crypto.Keccak256Hash(first) != tree.Hash() {
		t.Fatal("proof is not in root-to-leaf order")
	}
}

func TestActualMaintainedGethSnapNegotiationAndNetworkGate(t *testing.T) {
	req, _, _, _, account, proof, storage := witnessFixture(t)
	for _, wrongNetwork := range []bool{false, true} {
		t.Run(fmt.Sprint("wrong_network=", wrongNetwork), func(t *testing.T) {
			key, err := crypto.GenerateKey()
			if err != nil {
				t.Fatal(err)
			}
			chain := genesisChain{core.DefaultSepoliaGenesisBlock().ToBlock()}
			var requests atomic.Uint64
			network := networkID
			if wrongNetwork {
				network = 1
			}
			server := p2p.Server{Config: p2p.Config{PrivateKey: key, MaxPeers: 2, NoDiscovery: true, ListenAddr: "127.0.0.1:0", Logger: log.NewLogger(log.DiscardHandler()), Protocols: []p2p.Protocol{
				{Name: eth.ProtocolName, Version: eth.ETH69, Length: 18, Run: func(p *p2p.Peer, rw p2p.MsgReadWriter) error {
					peer := eth.NewPeer(eth.ETH69, p, rw, nil, nil, chain.Config())
					defer peer.Close()
					if err := peer.Handshake(network, chain, eth.BlockRangeUpdatePacket{LatestBlockHash: chain.block.Hash()}); err != nil {
						return err
					}
					for {
						m, err := rw.ReadMsg()
						if err != nil {
							return err
						}
						if err = m.Discard(); err != nil {
							return err
						}
					}
				}},
				{Name: snap.ProtocolName, Version: snap.SNAP1, Length: 8, Run: func(p *p2p.Peer, rw p2p.MsgReadWriter) error {
					m, err := rw.ReadMsg()
					if err != nil {
						return err
					}
					var q snap.GetAccountRangePacket
					if m.Code != snap.GetAccountRangeMsg {
						return errors.New("wrong SNAP protocol offset")
					}
					if err = m.Decode(&q); err != nil {
						return err
					}
					requests.Add(1)
					if err = p2p.Send(rw, snap.AccountRangeMsg, &snap.AccountRangePacket{ID: q.ID, Accounts: []*snap.AccountData{{Hash: q.Origin, Body: types.SlimAccountRLP(account)}}, Proof: proof}); err != nil {
						return err
					}
					m, err = rw.ReadMsg()
					if err != nil {
						return err
					}
					var q2 snap.GetStorageRangesPacket
					if m.Code != snap.GetStorageRangesMsg {
						return errors.New("wrong SNAP storage protocol offset")
					}
					if err = m.Decode(&q2); err != nil {
						return err
					}
					requests.Add(1)
					return p2p.Send(rw, snap.StorageRangesMsg, &snap.StorageRangesPacket{ID: q2.ID, Slots: [][]*snap.StorageData{{{Hash: common.BytesToHash(q2.Origin), Body: []byte{7}}}}, Proof: storage})
				}},
			}}}
			if err = server.Start(); err != nil {
				t.Fatal(err)
			}
			defer server.Stop()
			duration := 3 * time.Second
			if wrongNetwork {
				duration = time.Second
			}
			ctx, cancel := context.WithTimeout(context.Background(), duration)
			defer cancel()
			value, err := collectWitness(ctx, req, 1, server.Self().URLv4())
			if wrongNetwork {
				if err == nil || value.Passed || requests.Load() != 0 {
					t.Fatal("wrong-network peer reached SNAP authority path")
				}
				return
			}
			if err != nil || !value.Passed || value.Protocol != snap.SNAP1 || value.Handshakes != 1 || requests.Load() != 2 {
				t.Fatal("maintained Geth ETH/SNAP exchange failed", err, value)
			}
		})
	}
}

func TestSnapGateCannotTransferAcrossSameIdentityReconnect(t *testing.T) {
	var gates witnessGates
	id := enode.ID{77}
	first := p2p.NewPeer(id, "first connection", nil)
	reconnect := p2p.NewPeer(id, "same identity new connection", nil)
	accepted := gates.forPeer(first)
	accepted.once.Do(func() { close(accepted.ready) })
	if gates.forPeer(first) != accepted {
		t.Fatal("same physical connection lost readiness")
	}
	next := gates.forPeer(reconnect)
	if next == accepted {
		t.Fatal("readiness transferred to a distinct connection")
	}
	select {
	case <-next.ready:
		t.Fatal("reconnect bypassed its ETH network handshake")
	default:
	}
}
