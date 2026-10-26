// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license.
package main

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/ethereum/go-ethereum/common"
	"github.com/ethereum/go-ethereum/core"
	"github.com/ethereum/go-ethereum/core/types"
	"github.com/ethereum/go-ethereum/crypto"
	eth "github.com/ethereum/go-ethereum/eth/protocols/eth"
	snap "github.com/ethereum/go-ethereum/eth/protocols/snap"
	"github.com/ethereum/go-ethereum/log"
	"github.com/ethereum/go-ethereum/p2p"
	"github.com/ethereum/go-ethereum/p2p/dnsdisc"
	"github.com/ethereum/go-ethereum/p2p/enode"
	"github.com/ethereum/go-ethereum/params"
	"github.com/ethereum/go-ethereum/rlp"
	"github.com/ethereum/go-ethereum/trie"
)

const maxWitnessAccounts = 16
const maxWitnessSlots = 128
const maxProofNodes = 64
const maxProofNodeBytes = 8192
const maxWitnessBytes = 32 * 1024 * 1024
const maxSnapMessageBytes = 1024 * 1024

type witnessAccountRequest struct {
	Address     string   `json:"address"`
	StorageKeys []string `json:"storage_keys"`
}
type witnessRequest struct {
	StateRoot     string                  `json:"state_root,omitempty"`
	ExecutionHash string                  `json:"execution_hash,omitempty"`
	Accounts      []witnessAccountRequest `json:"accounts"`
}
type slotWitness struct {
	Key   string   `json:"slot"`
	Proof []string `json:"nodes"`
}
type accountWitness struct {
	Address      string        `json:"address"`
	AccountProof []string      `json:"nodes"`
	Storage      []slotWitness `json:"storage"`
}
type witnessResult struct {
	Format           string            `json:"format"`
	Passed           bool              `json:"passed"`
	ChainID          uint64            `json:"chain_id"`
	StateRoot        string            `json:"state_root"`
	Accounts         []accountWitness  `json:"accounts,omitempty"`
	AvailabilityOnly bool              `json:"availability_only"`
	NativeRequired   bool              `json:"native_verification_required"`
	Authority        bool              `json:"consensus_authority"`
	Mainnet          bool              `json:"mainnet_enabled"`
	Writes           bool              `json:"writes_enabled"`
	Geth             string            `json:"geth_dependency"`
	PeerSHA256       string            `json:"peer_id_sha256,omitempty"`
	Protocol         uint              `json:"snap_protocol_version,omitempty"`
	Seconds          float64           `json:"elapsed_seconds"`
	Connected        uint64            `json:"connected_peers"`
	Handshakes       uint64            `json:"matching_network_handshakes"`
	Failure          string            `json:"failure,omitempty"`
	FailureStages    map[string]uint64 `json:"failure_stages,omitempty"`
	ExecutionHash    string            `json:"execution_hash,omitempty"`
	Header           string            `json:"header_rlp,omitempty"`
	HeaderSHA256     string            `json:"header_sha256,omitempty"`
}

func baseWitness(req witnessRequest) witnessResult {
	return witnessResult{Format: "b3-ethereum-execution-peer-witness/1", ChainID: networkID, StateRoot: req.StateRoot, AvailabilityOnly: true, NativeRequired: true, Geth: gethVersion}
}

func canonicalBytes(value string, n int) ([]byte, error) {
	if len(value) != 2+n*2 || !strings.HasPrefix(value, "0x") || value != strings.ToLower(value) {
		return nil, errors.New("canonical fixed-size lowercase hex required")
	}
	out, err := hex.DecodeString(value[2:])
	if err != nil || len(out) != n {
		return nil, errors.New("invalid fixed-size hex")
	}
	return out, nil
}

// Go's JSON decoder accepts duplicate object members. Reject them before
// decoding the bounded request, so a saved request has one unambiguous meaning.
func uniqueWitnessJSON(d *json.Decoder, depth int) error {
	if depth > 8 {
		return errors.New("request nesting bound exceeded")
	}
	tok, err := d.Token()
	if err != nil {
		return err
	}
	delim, ok := tok.(json.Delim)
	if !ok {
		return nil
	}
	if delim == '{' {
		seen := map[string]bool{}
		for d.More() {
			key, err := d.Token()
			if err != nil {
				return err
			}
			s, ok := key.(string)
			if !ok || seen[s] {
				return errors.New("duplicate or invalid request member")
			}
			seen[s] = true
			if err = uniqueWitnessJSON(d, depth+1); err != nil {
				return err
			}
		}
	} else if delim == '[' {
		for d.More() {
			if err = uniqueWitnessJSON(d, depth+1); err != nil {
				return err
			}
		}
	} else {
		return errors.New("invalid JSON delimiter")
	}
	_, err = d.Token()
	return err
}
func decodeWitnessRequest(raw []byte) (witnessRequest, error) {
	var req witnessRequest
	if len(raw) == 0 || len(raw) > 65536 {
		return req, errors.New("witness request byte bound exceeded")
	}
	d := json.NewDecoder(bytes.NewReader(raw))
	if err := uniqueWitnessJSON(d, 0); err != nil {
		return req, err
	}
	if _, err := d.Token(); err != io.EOF {
		return req, errors.New("trailing request JSON")
	}
	d = json.NewDecoder(bytes.NewReader(raw))
	d.DisallowUnknownFields()
	if err := d.Decode(&req); err != nil {
		return req, err
	}
	if (req.StateRoot == "") == (req.ExecutionHash == "") {
		return req, errors.New("exactly one root or execution hash source required")
	}
	hashSource := req.StateRoot
	if req.ExecutionHash != "" {
		hashSource = req.ExecutionHash
	}
	if _, err := parseHash(hashSource); err != nil {
		return req, err
	}
	if len(req.Accounts) == 0 || len(req.Accounts) > maxWitnessAccounts {
		return req, errors.New("account count bound exceeded")
	}
	addresses := map[string]bool{}
	total := 0
	for _, a := range req.Accounts {
		b, err := canonicalBytes(a.Address, 20)
		if err != nil || common.BytesToAddress(b) == (common.Address{}) || addresses[a.Address] {
			return req, errors.New("invalid or duplicate account")
		}
		addresses[a.Address] = true
		seen := map[string]bool{}
		if len(a.StorageKeys) > 65 {
			return req, errors.New("per-account storage slot count exceeded")
		}
		for _, k := range a.StorageKeys {
			if _, err := canonicalBytes(k, 32); err != nil || seen[k] {
				return req, errors.New("invalid or duplicate storage key")
			}
			seen[k] = true
			total++
		}
	}
	if total > maxWitnessSlots {
		return req, errors.New("storage slot count bound exceeded")
	}
	return req, nil
}

// SNAP returns range boundary nodes in an arbitrary order. Native B3 accepts
// only the nodes actually consumed along the root-to-leaf path. Record Geth's
// independently checked reads and emit exactly those nodes in that order.
type orderedProofDB struct {
	nodes map[string][]byte
	used  [][]byte
	seen  map[string]bool
}

func (d *orderedProofDB) Has(key []byte) (bool, error) { _, ok := d.nodes[string(key)]; return ok, nil }
func (d *orderedProofDB) Get(key []byte) ([]byte, error) {
	b, ok := d.nodes[string(key)]
	if !ok {
		return nil, errors.New("missing proof node")
	}
	if !d.seen[string(key)] {
		d.seen[string(key)] = true
		d.used = append(d.used, b)
	}
	return b, nil
}
func orderedWitness(root, key common.Hash, nodes [][]byte) ([]string, []byte, error) {
	if len(nodes) > maxProofNodes {
		return nil, nil, errors.New("proof node count exceeded")
	}
	db := &orderedProofDB{nodes: map[string][]byte{}, seen: map[string]bool{}}
	for _, node := range nodes {
		if len(node) == 0 || len(node) > maxProofNodeBytes {
			return nil, nil, errors.New("proof node byte bound exceeded")
		}
		h := crypto.Keccak256Hash(node)
		if _, ok := db.nodes[string(h[:])]; ok {
			return nil, nil, errors.New("duplicate proof node")
		}
		db.nodes[string(h[:])] = append([]byte(nil), node...)
	}
	if root == types.EmptyRootHash {
		if len(nodes) != 0 {
			return nil, nil, errors.New("unexpected empty-trie proof")
		}
		return []string{}, nil, nil
	}
	value, err := trie.VerifyProof(root, key[:], db)
	if err != nil {
		return nil, nil, err
	}
	out := make([]string, 0, len(db.used))
	for _, node := range db.used {
		out = append(out, "0x"+hex.EncodeToString(node))
	}
	return out, value, nil
}
func accountRangeWitness(packet snap.AccountRangePacket, id uint64, root, key common.Hash) ([]string, types.StateAccount, error) {
	var account types.StateAccount
	if packet.ID != id || len(packet.Accounts) > 2 {
		return nil, account, errors.New("wrong account response")
	}
	proof, value, err := orderedWitness(root, key, packet.Proof)
	if err != nil {
		return nil, account, err
	}
	if len(value) == 0 || rlp.DecodeBytes(value, &account) != nil {
		return nil, account, errors.New("requested contract account absent or malformed")
	}
	// Range data is redundant but must agree if the peer returns this account.
	for _, a := range packet.Accounts {
		if a == nil || len(a.Body) > 512 {
			return nil, account, errors.New("malformed range account")
		}
		if a.Hash == key {
			full, err := types.FullAccountRLP(a.Body)
			if err != nil || !bytes.Equal(full, value) {
				return nil, account, errors.New("account range disagrees with proof")
			}
		}
	}
	return proof, account, nil
}
func storageRangeWitness(packet snap.StorageRangesPacket, id uint64, root, key common.Hash) ([]string, error) {
	if packet.ID != id || len(packet.Slots) > 1 {
		return nil, errors.New("wrong storage response")
	}
	proof, value, err := orderedWitness(root, key, packet.Proof)
	if err != nil {
		return nil, err
	}
	if len(value) > 33 {
		return nil, errors.New("oversize storage value")
	}
	for _, slots := range packet.Slots {
		if len(slots) > 2 {
			return nil, errors.New("storage range count exceeded")
		}
		for _, slot := range slots {
			if slot == nil || len(slot.Body) > 33 {
				return nil, errors.New("malformed storage range")
			}
			if slot.Hash == key && !bytes.Equal(slot.Body, value) {
				return nil, errors.New("storage range disagrees with proof")
			}
		}
	}
	return proof, nil
}

type boundedSnapRW struct {
	p2p.MsgReadWriter
	bytes    uint64
	messages uint64
}

func (rw *boundedSnapRW) ReadMsg() (p2p.Msg, error) {
	m, err := rw.MsgReadWriter.ReadMsg()
	if err != nil {
		return m, err
	}
	if m.Size > maxSnapMessageBytes || rw.bytes+uint64(m.Size) > maxWitnessBytes || rw.messages >= 256 {
		return p2p.Msg{}, errors.New("SNAP peer budget exceeded")
	}
	rw.bytes += uint64(m.Size)
	rw.messages++
	return m, nil
}
func (rw *boundedSnapRW) WriteMsg(m p2p.Msg) error {
	if m.Code != snap.GetAccountRangeMsg && m.Code != snap.GetStorageRangesMsg {
		return errors.New("read-only SNAP request refused")
	}
	return rw.MsgReadWriter.WriteMsg(m)
}
func witnessRequestID() (uint64, error) {
	var b [8]byte
	_, err := rand.Read(b[:])
	return binary.BigEndian.Uint64(b[:]), err
}

func readWitness(ctx context.Context, p *p2p.Peer, rw p2p.MsgReadWriter, version uint, req witnessRequest) (witnessResult, error) {
	value := baseWitness(req)
	bounded := &boundedSnapRW{MsgReadWriter: rw}
	root, _ := parseHash(req.StateRoot)
	for _, a := range req.Accounts {
		if err := ctx.Err(); err != nil {
			return value, err
		}
		address, _ := canonicalBytes(a.Address, 20)
		key := crypto.Keccak256Hash(address)
		id, err := witnessRequestID()
		if err != nil {
			return value, err
		}
		if err = p2p.Send(bounded, snap.GetAccountRangeMsg, &snap.GetAccountRangePacket{ID: id, Root: root, Origin: key, Limit: key, Bytes: 65536}); err != nil {
			return value, err
		}
		msg, err := bounded.ReadMsg()
		if err != nil {
			return value, err
		}
		if msg.Code != snap.AccountRangeMsg {
			return value, errors.New("unexpected SNAP account message")
		}
		var packet snap.AccountRangePacket
		if err = msg.Decode(&packet); err != nil {
			return value, err
		}
		proof, account, err := accountRangeWitness(packet, id, root, key)
		if err != nil {
			return value, err
		}
		witness := accountWitness{Address: a.Address, AccountProof: proof, Storage: []slotWitness{}}
		for _, k := range a.StorageKeys {
			if err = ctx.Err(); err != nil {
				return value, err
			}
			slot, _ := canonicalBytes(k, 32)
			slotHash := crypto.Keccak256Hash(slot)
			if account.Root == types.EmptyRootHash {
				witness.Storage = append(witness.Storage, slotWitness{Key: k, Proof: []string{}})
				continue
			}
			id, err = witnessRequestID()
			if err != nil {
				return value, err
			}
			if err = p2p.Send(bounded, snap.GetStorageRangesMsg, &snap.GetStorageRangesPacket{ID: id, Root: root, Accounts: []common.Hash{key}, Origin: slotHash[:], Limit: slotHash[:], Bytes: 65536}); err != nil {
				return value, err
			}
			msg, err = bounded.ReadMsg()
			if err != nil {
				return value, err
			}
			if msg.Code != snap.StorageRangesMsg {
				return value, errors.New("unexpected SNAP storage message")
			}
			var storage snap.StorageRangesPacket
			if err = msg.Decode(&storage); err != nil {
				return value, err
			}
			proof, err = storageRangeWitness(storage, id, account.Root, slotHash)
			if err != nil {
				return value, err
			}
			witness.Storage = append(witness.Storage, slotWitness{Key: k, Proof: proof})
		}
		value.Accounts = append(value.Accounts, witness)
	}
	raw, err := json.Marshal(value.Accounts)
	if err != nil || len(raw) > maxWitnessBytes {
		return baseWitness(req), errors.New("output witness byte bound exceeded")
	}
	value.Passed = true
	value.Protocol = version
	id := p.ID()
	digest := sha256.Sum256(id[:])
	value.PeerSHA256 = hex.EncodeToString(digest[:])
	return value, nil
}

type witnessGate struct {
	ready           chan struct{}
	once            sync.Once
	executionHeader []byte
	stateRoot       string
}

// A stable peer identity can reconnect. Readiness belongs to one physical
// connection, never to an identity whose prior connection passed ETH status.
type witnessGates struct{ connections sync.Map }

func (g *witnessGates) forPeer(connection *p2p.Peer) *witnessGate {
	value, _ := g.connections.LoadOrStore(connection, &witnessGate{ready: make(chan struct{})})
	return value.(*witnessGate)
}
func collectWitness(ctx context.Context, req witnessRequest, maxPeers int, fixture string) (witnessResult, error) {
	key, err := crypto.GenerateKey()
	if err != nil {
		return baseWitness(req), err
	}
	chain := genesisChain{core.DefaultSepoliaGenesisBlock().ToBlock()}
	if chain.block.Hash() != params.SepoliaGenesisHash {
		return baseWitness(req), errors.New("wrong genesis")
	}
	logger := log.NewLogger(log.DiscardHandler())
	var iterator enode.Iterator
	var nodes []*enode.Node
	if fixture != "" {
		n, err := enode.Parse(enode.ValidSchemes, fixture)
		if err != nil || !n.IP().Equal(net.ParseIP("127.0.0.1")) || n.TCP() < 1024 {
			return baseWitness(req), errors.New("owned fixture must be literal loopback")
		}
		nodes = []*enode.Node{n}
	} else {
		client := dnsdisc.NewClient(dnsdisc.Config{Timeout: 5 * time.Second, Logger: logger})
		it, err := client.NewIterator(params.KnownDNSNetwork(params.SepoliaGenesisHash, "all"))
		if err != nil {
			return baseWitness(req), err
		}
		iterator = &safeIterator{Iterator: it}
		defer iterator.Close()
		for _, url := range params.SepoliaBootnodes {
			n, err := enode.Parse(enode.ValidSchemes, url)
			if err != nil {
				return baseWitness(req), err
			}
			nodes = append(nodes, n)
		}
	}
	var gates witnessGates
	stats := new(counters)
	output := make(chan witnessResult, maxPeers)
	protocols := []p2p.Protocol{}
	for _, version := range []uint{eth.ETH69, eth.ETH70, eth.ETH71, eth.ETH72} {
		v := version
		length := uint64(18)
		if v == eth.ETH71 {
			length = 20
		}
		if v == eth.ETH72 {
			length = 22
		}
		protocols = append(protocols, p2p.Protocol{Name: eth.ProtocolName, Version: v, Length: length, DialCandidates: iterator, Run: func(p *p2p.Peer, rw p2p.MsgReadWriter) error {
			stats.Connected.Add(1)
			if !p.RunningCap(snap.ProtocolName, []uint{snap.SNAP1, snap.SNAP2}) {
				count, _ := stats.Stages.LoadOrStore("snap_capability_missing", new(atomic.Uint64))
				count.(*atomic.Uint64).Add(1)
				return errors.New("peer has no compatible SNAP extension")
			}
			bounded := &boundedRW{MsgReadWriter: rw}
			peer := eth.NewPeer(v, p, bounded, nil, nil, chain.Config())
			defer peer.Close()
			timer := time.AfterFunc(90*time.Second, func() { p.Disconnect(p2p.DiscReadTimeout) })
			defer timer.Stop()
			if err := peer.Handshake(networkID, chain, eth.BlockRangeUpdatePacket{LatestBlockHash: chain.block.Hash()}); err != nil {
				value, _ := stats.Stages.LoadOrStore("eth_handshake_"+peerDropClass(err.Error()), new(atomic.Uint64))
				value.(*atomic.Uint64).Add(1)
				return err
			}
			stats.Handshakes.Add(1)
			g := gates.forPeer(p)
			g.stateRoot = req.StateRoot
			if req.ExecutionHash != "" {
				want, _ := parseHash(req.ExecutionHash)
				raw, err := witnessExecutionHeader(ctx, bounded, want, v)
				if err != nil {
					count, _ := stats.Stages.LoadOrStore("exact_header_"+peerDropClass(err.Error()), new(atomic.Uint64))
					count.(*atomic.Uint64).Add(1)
					return err
				}
				var header types.Header
				if err = rlp.DecodeBytes(raw, &header); err != nil || header.Root == (common.Hash{}) {
					return errors.New("malformed exact header state root")
				}
				g.executionHeader = raw
				g.stateRoot = header.Root.Hex()
			}
			g.once.Do(func() { close(g.ready) })
			for {
				if err := ctx.Err(); err != nil {
					return err
				}
				m, err := bounded.ReadMsg()
				if err != nil {
					return err
				}
				switch m.Code {
				case eth.GetBlockHeadersMsg:
					var q eth.GetBlockHeadersPacket
					if err = m.Decode(&q); err != nil {
						return err
					}
					if err = p2p.Send(bounded, eth.BlockHeadersMsg, &eth.BlockHeadersRLPPacket{RequestId: q.RequestId}); err != nil {
						return err
					}
				case eth.GetBlockBodiesMsg:
					var q eth.GetBlockBodiesPacket
					if err = m.Decode(&q); err != nil {
						return err
					}
					if err = p2p.Send(bounded, eth.BlockBodiesMsg, &eth.BlockBodiesRLPPacket{RequestId: q.RequestId}); err != nil {
						return err
					}
				case eth.NewPooledTransactionHashesMsg, eth.TransactionsMsg, eth.BlockRangeUpdateMsg:
					if err = m.Discard(); err != nil {
						return err
					}
				case eth.GetReceiptsMsg, eth.GetBlockAccessListsMsg, eth.GetCellsMsg:
					if _, err = replyUnavailableETHData(m, bounded, v); err != nil {
						return err
					}
				default:
					return errors.New("unexpected ETH witness companion message")
				}
			}
		}})
	}
	for _, version := range []uint{snap.SNAP1, snap.SNAP2} {
		v := version
		length := uint64(8)
		if v == snap.SNAP2 {
			length = 10
		}
		protocols = append(protocols, p2p.Protocol{Name: snap.ProtocolName, Version: v, Length: length, Run: func(p *p2p.Peer, rw p2p.MsgReadWriter) error {
			timer := time.AfterFunc(90*time.Second, func() { p.Disconnect(p2p.DiscReadTimeout) })
			defer timer.Stop()
			peerCtx, cancel := context.WithCancel(ctx)
			defer cancel()
			// Drain/answer inbound requests before waiting for ETH status.
			// Only our outbound proof queries wait for the network gate.
			duplex := newWitnessDuplex(peerCtx, rw, v)
			select {
			case <-gates.forPeer(p).ready:
			case <-duplex.done:
				return duplex.Err()
			case <-ctx.Done():
				return ctx.Err()
			}
			g := gates.forPeer(p)
			boundRequest := req
			boundRequest.StateRoot = g.stateRoot
			value, err := readWitness(peerCtx, p, duplex, v, boundRequest)
			if err != nil {
				stage := "snap_other"
				for _, kind := range []string{"proof", "missing", "unexpected", "wrong", "EOF", "budget", "context"} {
					if strings.Contains(err.Error(), kind) {
						stage = "snap_" + strings.ToLower(kind)
						break
					}
				}
				count, _ := stats.Stages.LoadOrStore(stage, new(atomic.Uint64))
				count.(*atomic.Uint64).Add(1)
				return err
			}
			if req.ExecutionHash != "" {
				value.ExecutionHash = req.ExecutionHash
				value.Header = "0x" + hex.EncodeToString(g.executionHeader)
				digest := sha256.Sum256(g.executionHeader)
				value.HeaderSHA256 = hex.EncodeToString(digest[:])
			}
			encoded, marshalErr := json.Marshal(value)
			if marshalErr != nil || len(encoded) > maxWitnessBytes {
				return errors.New("combined witness output budget exceeded")
			}
			select {
			case output <- value:
			case <-ctx.Done():
				return ctx.Err()
			}
			return nil
		}})
	}
	server := p2p.Server{Config: p2p.Config{PrivateKey: key, MaxPeers: maxPeers, MaxPendingPeers: 4, DialRatio: 1, Name: "b3-execution-witness/1", ListenAddr: "127.0.0.1:0", DiscAddr: "0.0.0.0:0", NoDiscovery: fixture != "", DiscoveryV4: fixture == "", BootstrapNodes: nodes, StaticNodes: fixtureStaticNodes(fixture, nodes), Protocols: protocols, Dialer: publicDialer{fixture: fixture != "", dialer: net.Dialer{Timeout: 5 * time.Second}}, Logger: logger}}
	if err = server.Start(); err != nil {
		return baseWitness(req), err
	}
	defer server.Stop()
	events := make(chan *p2p.PeerEvent, 64)
	subscription := server.SubscribeEvents(events)
	defer subscription.Unsubscribe()
	eventCtx, eventCancel := context.WithCancel(ctx)
	defer eventCancel()
	go func() {
		for {
			select {
			case event := <-events:
				countPeerDrop(stats, event)
			case <-eventCtx.Done():
				return
			}
		}
	}()
	var result witnessResult
	select {
	case result = <-output:
	case <-ctx.Done():
		result = baseWitness(req)
		err = errors.New("no complete witness within peer/time budgets")
	}
	result.Connected = stats.Connected.Load()
	result.Handshakes = stats.Handshakes.Load()
	result.FailureStages = map[string]uint64{}
	stats.Stages.Range(func(k, v any) bool { result.FailureStages[fmt.Sprint(k)] = v.(*atomic.Uint64).Load(); return true })
	return result, err
}
func readWitnessRequestFile(path string) ([]byte, error) {
	info, err := os.Lstat(path)
	if err != nil || !info.Mode().IsRegular() || info.Size() > 65536 {
		return nil, errors.New("bounded regular witness request required")
	}
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	opened, err := file.Stat()
	if err != nil || !os.SameFile(info, opened) {
		return nil, errors.New("request changed while opening")
	}
	return io.ReadAll(io.LimitReader(file, 65537))
}
func runWitness(requestPath string, seconds, maxPeers int, fixture string) int {
	value := baseWitness(witnessRequest{})
	started := time.Now()
	status := 1
	if seconds < 1 || seconds > 300 || maxPeers < 1 || maxPeers > 8 {
		value.Failure = "invalid bounded options"
	} else if raw, err := readWitnessRequestFile(requestPath); err != nil {
		value.Failure = "cannot read bounded witness request"
	} else if req, err := decodeWitnessRequest(raw); err != nil {
		value.Failure = "malformed bounded witness request"
	} else {
		ctx, cancel := context.WithTimeout(context.Background(), time.Duration(seconds)*time.Second)
		defer cancel()
		value, err = collectWitness(ctx, req, maxPeers, fixture)
		if err != nil {
			value.Failure = "SNAP proof availability refused or timed out"
		} else {
			status = 0
		}
	}
	value.Seconds = time.Since(started).Seconds()
	raw, encodeErr := encodeAvailabilityJSON(value)
	if encodeErr != nil {
		value = baseWitness(witnessRequest{})
		value.Failure = "final witness output byte bound exceeded"
		raw, _ = encodeAvailabilityJSON(value)
		status = 1
	}
	if _, writeErr := os.Stdout.Write(raw); writeErr != nil {
		return 1
	}
	return status
}
