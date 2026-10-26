// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license.
package main

import (
	"bytes"
	"errors"
	"io"

	eth "github.com/ethereum/go-ethereum/eth/protocols/eth"
	"github.com/ethereum/go-ethereum/p2p"
	"github.com/ethereum/go-ethereum/rlp"
)

// Decode only bounded, exactly framed read-only availability requests. The
// caller still owns the physical peer's aggregate byte/message/deadline bound.
func decodeUnavailableETH(m p2p.Msg, out any) error {
	if m.Payload == nil || m.Size > maxMessageBytes {
		return errors.New("unavailable ETH request byte bound or payload")
	}
	raw, err := io.ReadAll(io.LimitReader(m.Payload, int64(m.Size)+1))
	if err != nil || len(raw) != int(m.Size) {
		return errors.New("incomplete unavailable ETH request")
	}
	return rlp.DecodeBytes(raw, out)
}

// A data requester has no receipts, access lists or blob cells to serve. Empty
// replies preserve request IDs and protocol versions without advertising data
// or granting consensus authority. Unknown/response messages stay unhandled.
func replyUnavailableETHData(m p2p.Msg, rw p2p.MsgReadWriter, version uint) (bool, error) {
	if version < eth.ETH69 || version > eth.ETH72 {
		return true, errors.New("unsupported unavailable ETH version")
	}
	switch m.Code {
	case eth.GetReceiptsMsg:
		if version == eth.ETH69 {
			var q eth.GetReceiptsPacket69
			if err := decodeUnavailableETH(m, &q); err != nil {
				return true, err
			}
			return true, p2p.Send(rw, eth.ReceiptsMsg, &eth.ReceiptsPacket69{RequestId: q.RequestId})
		}
		var q eth.GetReceiptsPacket70
		if err := decodeUnavailableETH(m, &q); err != nil {
			return true, err
		}
		return true, p2p.Send(rw, eth.ReceiptsMsg, &eth.ReceiptsPacket70{RequestId: q.RequestId})
	case eth.GetBlockAccessListsMsg:
		if version < eth.ETH71 {
			return true, errors.New("access list request outside ETH71")
		}
		var q eth.GetBlockAccessListsPacket
		if err := decodeUnavailableETH(m, &q); err != nil {
			return true, err
		}
		// Geth/EIP-8159 uses one empty STRING per unavailable block, because an
		// empty list itself is a valid BAL. Follow its maximum 1024 response items.
		var list rlp.RawList[rlp.RawValue]
		for i := 0; i < len(q.GetBlockAccessListsRequest) && i < 1024; i++ {
			list.AppendRaw(rlp.EmptyString)
		}
		return true, p2p.Send(rw, eth.BlockAccessListsMsg, &eth.BlockAccessListPacket{RequestId: q.RequestId, List: list})
	case eth.GetCellsMsg:
		if version != eth.ETH72 {
			return true, errors.New("cell request outside ETH72")
		}
		var q eth.GetCellsRequestPacket
		if err := decodeUnavailableETH(m, &q); err != nil {
			return true, err
		}
		return true, p2p.Send(rw, eth.CellsMsg, &eth.CellsPacket{RequestId: q.RequestId, Mask: q.Mask})
	}
	return false, nil
}

// Additional outbound reply codes are admitted only with unavailable payloads.
// A response code alone must never turn a read-only transport into a data or
// transaction sender. The payload is restored for the underlying writer.
func unavailableETHResponse(code uint64) bool {
	return code == eth.ReceiptsMsg || code == eth.BlockAccessListsMsg || code == eth.CellsMsg
}
func validateUnavailableETHReply(m *p2p.Msg) error {
	if m.Payload == nil || !unavailableETHResponse(m.Code) || m.Size > 16*1024 {
		return errors.New("unavailable ETH reply bound or code")
	}
	raw, err := io.ReadAll(io.LimitReader(m.Payload, int64(m.Size)+1))
	if err != nil || len(raw) != int(m.Size) {
		return errors.New("incomplete unavailable ETH reply")
	}
	valid := false
	switch m.Code {
	case eth.ReceiptsMsg:
		var old eth.ReceiptsPacket69
		var current eth.ReceiptsPacket70
		valid = (rlp.DecodeBytes(raw, &old) == nil && old.List.Len() == 0) || (rlp.DecodeBytes(raw, &current) == nil && current.List.Len() == 0 && !current.LastBlockIncomplete)
	case eth.BlockAccessListsMsg:
		var p eth.BlockAccessListPacket
		if rlp.DecodeBytes(raw, &p) == nil && p.List.Len() <= 1024 {
			items, e := p.List.Items()
			valid = e == nil
			for _, item := range items {
				if !bytes.Equal(item, rlp.EmptyString) {
					valid = false
				}
			}
		}
	case eth.CellsMsg:
		var p eth.CellsPacket
		valid = rlp.DecodeBytes(raw, &p) == nil && len(p.Hashes) == 0 && p.Cells.Len() == 0
	}
	if !valid {
		return errors.New("outbound ETH reply is not unavailable")
	}
	m.Payload = bytes.NewReader(raw)
	return nil
}
