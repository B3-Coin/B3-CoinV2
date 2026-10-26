// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license.

// Read-only Ethereum Sepolia header availability over maintained Geth RLPx.
// A matching header hash is not consensus authority. The native verifier must
// authenticate the requested hash before using its state or receipt roots.
package main

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"net"
	"os"
	"strconv"
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
)

const networkID uint64 = 11155111
const gethVersion = "v1.17.10"
const maxHeaderBytes = 4096
const maxMessageBytes = 128 * 1024
const maxPeerBytes = 4 * 1024 * 1024
const maxMessages = 256

type counters struct {
	Connected, Handshakes, Refusals atomic.Uint64
	Stages                          sync.Map
}
type result struct {
	Format                  string            `json:"format"`
	Passed                  bool              `json:"passed"`
	ChainID                 uint64            `json:"chain_id"`
	Genesis                 string            `json:"genesis_hash"`
	RequestedHash           string            `json:"requested_hash,omitempty"`
	Header                  string            `json:"header_rlp,omitempty"`
	HeaderSHA256            string            `json:"header_sha256,omitempty"`
	PeerSHA256              string            `json:"peer_id_sha256,omitempty"`
	Protocol                uint              `json:"protocol_version,omitempty"`
	Source                  string            `json:"requested_hash_source,omitempty"`
	Geth                    string            `json:"geth_dependency"`
	AvailabilityOnly        bool              `json:"availability_only"`
	Authority               bool              `json:"consensus_authority"`
	NativeRequired          bool              `json:"native_verification_required"`
	Mainnet                 bool              `json:"mainnet_enabled"`
	Seconds                 float64           `json:"elapsed_seconds"`
	Connected               uint64            `json:"connected_peers"`
	Handshakes              uint64            `json:"matching_network_handshakes"`
	Refusals                uint64            `json:"refused_peers"`
	FailureStages           map[string]uint64 `json:"failure_stages,omitempty"`
	Failure                 string            `json:"failure,omitempty"`
	BlockNumber             uint64            `json:"block_number,omitempty"`
	BlockHash               string            `json:"block_hash,omitempty"`
	ReceiptsRoot            string            `json:"receipts_root,omitempty"`
	Receipts                []string          `json:"receipts,omitempty"`
	ReceiptCommitmentsMatch bool              `json:"receipt_commitments_match,omitempty"`
}

func baseResult() result {
	return result{Format: "b3-ethereum-execution-peer-header/1", ChainID: networkID,
		Genesis: params.SepoliaGenesisHash.Hex(), Geth: gethVersion,
		AvailabilityOnly: true, NativeRequired: true}
}

func parseHash(value string) (common.Hash, error) {
	if len(value) != 66 || !strings.HasPrefix(value, "0x") || value != strings.ToLower(value) {
		return common.Hash{}, errors.New("canonical lowercase 32-byte hash required")
	}
	b, err := hex.DecodeString(value[2:])
	if err != nil || len(b) != 32 || common.BytesToHash(b) == (common.Hash{}) {
		return common.Hash{}, errors.New("nonzero 32-byte hash required")
	}
	return common.BytesToHash(b), nil
}

// The client knows only the network genesis. It honestly advertises no blocks
// beyond genesis; Geth's fork filter permits compatible peers ahead of it.
type genesisChain struct{ block *types.Block }

func (c genesisChain) Config() *params.ChainConfig  { return params.SepoliaChainConfig }
func (c genesisChain) Genesis() *types.Block        { return c.block }
func (c genesisChain) CurrentHeader() *types.Header { return c.block.Header() }

type boundedRW struct {
	p2p.MsgReadWriter
	bytes    uint64
	messages uint64
}

func (rw *boundedRW) ReadMsg() (p2p.Msg, error) {
	m, err := rw.MsgReadWriter.ReadMsg()
	if err != nil {
		return m, err
	}
	if m.Size > maxMessageBytes || rw.bytes+uint64(m.Size) > maxPeerBytes || rw.messages >= maxMessages {
		return p2p.Msg{}, errors.New("peer message budget exceeded")
	}
	rw.bytes += uint64(m.Size)
	rw.messages++
	return m, nil
}
func (rw *boundedRW) WriteMsg(m p2p.Msg) error {
	if unavailableETHResponse(m.Code) {
		if err := validateUnavailableETHReply(&m); err != nil {
			return err
		}
		return rw.MsgReadWriter.WriteMsg(m)
	}
	switch m.Code {
	case eth.StatusMsg, eth.GetBlockHeadersMsg, eth.BlockHeadersMsg, eth.BlockBodiesMsg:
		return rw.MsgReadWriter.WriteMsg(m)
	default:
		return errors.New("read-only peer transport refuses outbound message")
	}
}

// Only exact bytes committed by the requested hash can leave this transport.
// The native Gloas verifier still independently checks this same preimage.
func acceptHeader(packet eth.BlockHeadersRLPPacket, requestID uint64, want common.Hash) ([]byte, error) {
	if packet.RequestId != requestID || len(packet.BlockHeadersRLPResponse) != 1 {
		return nil, errors.New("wrong request or incomplete header reply")
	}
	raw := packet.BlockHeadersRLPResponse[0]
	if len(raw) == 0 || len(raw) > maxHeaderBytes || crypto.Keccak256Hash(raw) != want {
		return nil, errors.New("raw header does not match requested hash")
	}
	var header types.Header
	if rlp.DecodeBytes(raw, &header) != nil || header.Number == nil || header.Number.Sign() < 0 || header.Number.BitLen() > 64 {
		return nil, errors.New("unsupported or malformed Geth header")
	}
	return append([]byte(nil), raw...), nil
}

type publicDialer struct {
	fixture bool
	dialer  net.Dialer
}

func (d publicDialer) Dial(ctx context.Context, node *enode.Node) (net.Conn, error) {
	ip := node.IP()
	allowed := ip != nil && ip.IsGlobalUnicast() && !ip.IsPrivate() && !ip.IsLoopback()
	if d.fixture {
		allowed = ip != nil && ip.Equal(net.ParseIP("127.0.0.1")) && node.TCP() >= 1024
	}
	if !allowed {
		return nil, errors.New("peer address outside selected availability boundary")
	}
	return d.dialer.DialContext(ctx, "tcp", net.JoinHostPort(ip.String(), strconv.Itoa(node.TCP())))
}

type safeIterator struct {
	enode.Iterator
	once sync.Once
}

func (i *safeIterator) Close() { i.once.Do(i.Iterator.Close) }

func readPeer(ctx context.Context, p *p2p.Peer, rw p2p.MsgReadWriter, version uint, chain genesisChain,
	want common.Hash, untrustedTip bool, output chan<- result, stats *counters) (returned error) {
	phase := "handshake"
	defer func() {
		if returned != nil {
			counter, _ := stats.Stages.LoadOrStore(phase, new(atomic.Uint64))
			counter.(*atomic.Uint64).Add(1)
		}
	}()
	stats.Connected.Add(1)
	bounded := &boundedRW{MsgReadWriter: rw}
	peer := eth.NewPeer(version, p, bounded, nil, nil, chain.Config())
	defer peer.Close()
	timer := time.AfterFunc(20*time.Second, func() { p.Disconnect(p2p.DiscReadTimeout) })
	defer timer.Stop()
	if err := peer.Handshake(networkID, chain, eth.BlockRangeUpdatePacket{EarliestBlock: 0, LatestBlock: 0, LatestBlockHash: chain.block.Hash()}); err != nil {
		phase = "handshake_other"
		for _, kind := range []string{"fork", "genesis", "network", "status", "timeout", "EOF", "disconnect"} {
			if strings.Contains(err.Error(), kind) {
				phase = "handshake_" + kind
				break
			}
		}
		stats.Refusals.Add(1)
		return err
	}
	stats.Handshakes.Add(1)
	phase = "request"
	source := "separately-supplied-untrusted-hash"
	if untrustedTip {
		want = peer.BlockRange().LatestBlockHash
		source = "untrusted-peer-status-tip"
	}
	var random [8]byte
	if _, err := rand.Read(random[:]); err != nil {
		return err
	}
	requestID := binary.BigEndian.Uint64(random[:])
	request := eth.GetBlockHeadersPacket{RequestId: requestID, GetBlockHeadersRequest: &eth.GetBlockHeadersRequest{
		Origin: eth.HashOrNumber{Hash: want}, Amount: 1, Skip: 0, Reverse: false}}
	if err := p2p.Send(bounded, eth.GetBlockHeadersMsg, &request); err != nil {
		return err
	}
	for {
		phase = "read_message"
		if err := ctx.Err(); err != nil {
			return err
		}
		message, err := bounded.ReadMsg()
		if err != nil {
			return err
		}
		switch message.Code {
		case eth.BlockHeadersMsg:
			phase = "decode_header_response"
			var packet eth.BlockHeadersRLPPacket
			if err = message.Decode(&packet); err != nil {
				return err
			}
			phase = "header_hash_or_structure"
			raw, err := acceptHeader(packet, requestID, want)
			if err != nil {
				stats.Refusals.Add(1)
				return err
			}
			response := baseResult()
			response.Passed = true
			response.RequestedHash = want.Hex()
			response.Header = "0x" + hex.EncodeToString(raw)
			response.Source = source
			response.Protocol = version
			digest := sha256.Sum256(raw)
			response.HeaderSHA256 = hex.EncodeToString(digest[:])
			id := p.ID()
			digest = sha256.Sum256(id[:])
			response.PeerSHA256 = hex.EncodeToString(digest[:])
			select {
			case output <- response:
			case <-ctx.Done():
			}
			return nil
		case eth.GetBlockHeadersMsg:
			var query eth.GetBlockHeadersPacket
			if err = message.Decode(&query); err != nil {
				return err
			}
			if err = p2p.Send(bounded, eth.BlockHeadersMsg, &eth.BlockHeadersRLPPacket{RequestId: query.RequestId}); err != nil {
				return err
			}
		case eth.GetBlockBodiesMsg:
			var query eth.GetBlockBodiesPacket
			if err = message.Decode(&query); err != nil {
				return err
			}
			if err = p2p.Send(bounded, eth.BlockBodiesMsg, &eth.BlockBodiesRLPPacket{RequestId: query.RequestId}); err != nil {
				return err
			}
		case eth.NewPooledTransactionHashesMsg, eth.TransactionsMsg, eth.BlockRangeUpdateMsg:
			if err = message.Discard(); err != nil {
				return err
			}
		case eth.GetReceiptsMsg, eth.GetBlockAccessListsMsg, eth.GetCellsMsg:
			if _, err = replyUnavailableETHData(message, bounded, version); err != nil {
				return err
			}
		default:
			phase = "unexpected_message"
			return errors.New("unexpected read-only peer message")
		}
	}
}

type peerTask func(context.Context, *p2p.Peer, p2p.MsgReadWriter, uint, genesisChain, chan<- result, *counters) error

func collect(ctx context.Context, want common.Hash, tip bool, maxPeers int, fixture string, tasks ...peerTask) (result, error) {
	if len(tasks) > 1 {
		return baseResult(), errors.New("one availability task required")
	}

	key, err := crypto.GenerateKey()
	if err != nil {
		return baseResult(), err
	}
	genesis := core.DefaultSepoliaGenesisBlock().ToBlock()
	if genesis.Hash() != params.SepoliaGenesisHash {
		return baseResult(), errors.New("pinned Sepolia genesis mismatch")
	}
	chain := genesisChain{genesis}
	stats := new(counters)
	output := make(chan result, maxPeers)
	logger := log.NewLogger(log.DiscardHandler())
	var iterator enode.Iterator
	var nodes []*enode.Node
	if fixture != "" {
		n, err := enode.Parse(enode.ValidSchemes, fixture)
		if err != nil || !n.IP().Equal(net.ParseIP("127.0.0.1")) || n.TCP() < 1024 {
			return baseResult(), errors.New("owned fixture peer must be literal loopback")
		}
		nodes = []*enode.Node{n}
	} else {
		client := dnsdisc.NewClient(dnsdisc.Config{Timeout: 5 * time.Second, Logger: logger})
		discovered, e := client.NewIterator(params.KnownDNSNetwork(params.SepoliaGenesisHash, "all"))
		if e != nil {
			return baseResult(), e
		}
		iterator = &safeIterator{Iterator: discovered}
		defer iterator.Close()
		for _, url := range params.SepoliaBootnodes {
			n, err := enode.Parse(enode.ValidSchemes, url)
			if err != nil {
				return baseResult(), err
			}
			nodes = append(nodes, n)
		}
	}
	protocols := make([]p2p.Protocol, 0, 4)
	for _, version := range []uint{eth.ETH69, eth.ETH70, eth.ETH71, eth.ETH72} {
		v := version
		length := uint64(18)
		if v == eth.ETH71 {
			length = 20
		}
		if v == eth.ETH72 {
			length = 22
		}
		protocols = append(protocols, p2p.Protocol{Name: eth.ProtocolName, Version: v, Length: length, DialCandidates: iterator,
			Run: func(p *p2p.Peer, rw p2p.MsgReadWriter) error {
				if len(tasks) == 1 {
					return tasks[0](ctx, p, rw, v, chain, output, stats)
				}
				return readPeer(ctx, p, rw, v, chain, want, tip, output, stats)
			}})
	}
	for _, version := range []uint{snap.SNAP1, snap.SNAP2} {
		v := version
		length := uint64(8)
		if v == snap.SNAP2 {
			length = 10
		}
		protocols = append(protocols, p2p.Protocol{Name: snap.ProtocolName, Version: v, Length: length,
			Run: func(p *p2p.Peer, rw p2p.MsgReadWriter) error { return serveUnavailableSnap(rw, v) }})
	}
	server := p2p.Server{Config: p2p.Config{PrivateKey: key, MaxPeers: maxPeers, MaxPendingPeers: 4, DialRatio: 1,
		Name: "b3-execution-header/1", ListenAddr: "127.0.0.1:0", DiscAddr: "0.0.0.0:0",
		NoDiscovery: fixture != "", DiscoveryV4: fixture == "", BootstrapNodes: nodes, StaticNodes: fixtureStaticNodes(fixture, nodes),
		Protocols: protocols, Dialer: publicDialer{fixture: fixture != "", dialer: net.Dialer{Timeout: 5 * time.Second}}, Logger: logger}}
	if err = server.Start(); err != nil {
		return baseResult(), err
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
	var value result
	select {
	case value = <-output:
	case <-ctx.Done():
		value = baseResult()
		err = errors.New("no requested header within peer/time budgets")
	}
	value.Connected = stats.Connected.Load()
	value.Handshakes = stats.Handshakes.Load()
	value.Refusals = stats.Refusals.Load()
	value.FailureStages = make(map[string]uint64)
	stats.Stages.Range(func(k, v any) bool { value.FailureStages[k.(string)] = v.(*atomic.Uint64).Load(); return true })
	return value, err
}

func run() int {
	var requested string
	var seconds, maxPeers int
	var tip bool
	var fixture string
	var witnessFile string
	var receiptsBlock string
	flag.StringVar(&requested, "hash", "", "exact expected execution header hash; not itself consensus authority")
	flag.BoolVar(&tip, "untrusted-peer-tip", false, "availability diagnostic only: request an untrusted peer status tip")
	flag.IntVar(&seconds, "seconds", 90, "total bounded peer discovery/request time (1..300)")
	flag.IntVar(&maxPeers, "max-peers", 4, "bounded connected peer count (1..8)")
	flag.StringVar(&fixture, "owned-loopback-peer", "", "test-only exact loopback enode; disables public discovery")
	flag.StringVar(&witnessFile, "witness-request", "", "bounded data-only account/storage witness request file")
	flag.StringVar(&receiptsBlock, "receipts-block", "", "untrusted positive Sepolia block number for bounded receipt availability")
	flag.Parse()
	value := baseResult()
	if receiptsBlock != "" {
		if receiptModeConflict(requested, tip, witnessFile) || flag.NArg() != 0 {
			out := receiptResult()
			out.Failure = "receipt request requires its own bounded options"
			json.NewEncoder(os.Stdout).Encode(out)
			return 2
		}
		return runReceiptBlock(receiptsBlock, seconds, maxPeers, fixture)
	}
	if witnessFile != "" {
		if requested != "" || tip || flag.NArg() != 0 || seconds < 1 || seconds > 300 || maxPeers < 1 || maxPeers > 8 {
			value.Failure = "witness request requires its own bounded options"
			json.NewEncoder(os.Stdout).Encode(value)
			return 2
		}
		return runWitness(witnessFile, seconds, maxPeers, fixture)
	}
	if flag.NArg() != 0 || seconds < 1 || seconds > 300 || maxPeers < 1 || maxPeers > 8 || (requested == "") == !tip {
		value.Failure = "exactly one hash source and valid bounded options required"
		json.NewEncoder(os.Stdout).Encode(value)
		return 2
	}
	var hash common.Hash
	var err error
	if requested != "" {
		hash, err = parseHash(requested)
		if err != nil {
			value.Failure = err.Error()
			json.NewEncoder(os.Stdout).Encode(value)
			return 2
		}
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Duration(seconds)*time.Second)
	defer cancel()
	started := time.Now()
	value, err = collect(ctx, hash, tip, maxPeers, fixture)
	value.Seconds = time.Since(started).Seconds()
	if err != nil {
		value.Failure = "execution peer availability refused or timed out"
	}
	if err = json.NewEncoder(os.Stdout).Encode(value); err != nil {
		return 1
	}
	if !value.Passed {
		return 1
	}
	return 0
}

func main() { os.Exit(run()) }
