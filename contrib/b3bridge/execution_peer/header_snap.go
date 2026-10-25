// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license.
package main

import (
	"errors"
	"strings"
	"sync/atomic"

	snap "github.com/ethereum/go-ethereum/eth/protocols/snap"
	"github.com/ethereum/go-ethereum/p2p"
)

// A header requester has no state to serve. SNAP explicitly permits empty
// replies for unavailable roots. Supporting this maintained protocol extension
// avoids an ETH-only admission penalty while honestly offering no state data.
// It does not manufacture a synchronized head, change fork filters, or send
// transactions. All protocol message offsets are handled by p2p.Server.
func replyUnavailableSnap(msg p2p.Msg, rw p2p.MsgReadWriter, version uint) (bool, error) {
	switch msg.Code {
	case snap.GetAccountRangeMsg:
		var q snap.GetAccountRangePacket
		if err := msg.Decode(&q); err != nil {
			return true, err
		}
		return true, p2p.Send(rw, snap.AccountRangeMsg, &snap.AccountRangePacket{ID: q.ID})
	case snap.GetStorageRangesMsg:
		var q snap.GetStorageRangesPacket
		if err := msg.Decode(&q); err != nil {
			return true, err
		}
		return true, p2p.Send(rw, snap.StorageRangesMsg, &snap.StorageRangesPacket{ID: q.ID})
	case snap.GetByteCodesMsg:
		var q snap.GetByteCodesPacket
		if err := msg.Decode(&q); err != nil {
			return true, err
		}
		return true, p2p.Send(rw, snap.ByteCodesMsg, &snap.ByteCodesPacket{ID: q.ID})
	case snap.GetTrieNodesMsg:
		var q snap.GetTrieNodesPacket
		if err := msg.Decode(&q); err != nil {
			return true, err
		}
		return true, p2p.Send(rw, snap.TrieNodesMsg, &snap.TrieNodesPacket{ID: q.ID})
	case snap.GetAccessListsMsg:
		if version != snap.SNAP2 {
			return true, errors.New("access list request outside SNAP2")
		}
		var q snap.GetAccessListsPacket
		if err := msg.Decode(&q); err != nil {
			return true, err
		}
		return true, p2p.Send(rw, snap.AccessListsMsg, &snap.AccessListsPacket{ID: q.ID})
	}
	return false, nil
}
func serveUnavailableSnap(rw p2p.MsgReadWriter, version uint) error {
	var total uint64
	for count := 0; count < 256; count++ {
		msg, err := rw.ReadMsg()
		if err != nil {
			return err
		}
		if msg.Size > maxSnapMessageBytes || total+uint64(msg.Size) > maxPeerBytes {
			return errors.New("SNAP availability request budget exceeded")
		}
		total += uint64(msg.Size)
		handled, err := replyUnavailableSnap(msg, rw, version)
		if err != nil {
			return err
		}
		if !handled {
			return errors.New("unexpected read-only SNAP message")
		}
	}
	return errors.New("SNAP availability message count exceeded")
}
func peerDropClass(message string) string {
	lower := strings.ToLower(message)
	for _, kind := range []string{"too many peers", "useless peer", "incompatible", "protocol error", "network error", "disconnect requested", "read timeout", "client quitting", "unexpected identity", "subprotocol error", "already connected", "eof"} {
		if strings.Contains(lower, kind) {
			return strings.ReplaceAll(kind, " ", "_")
		}
	}
	return "other"
}
func countPeerDrop(stats *counters, event *p2p.PeerEvent) {
	if event.Type != p2p.PeerEventTypeDrop {
		return
	}
	value, _ := stats.Stages.LoadOrStore("peer_drop_"+peerDropClass(event.Error), new(atomic.Uint64))
	value.(*atomic.Uint64).Add(1)
}
