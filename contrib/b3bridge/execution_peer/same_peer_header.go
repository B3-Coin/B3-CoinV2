// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license.
package main

import (
	"context"
	"errors"

	"github.com/ethereum/go-ethereum/common"
	eth "github.com/ethereum/go-ethereum/eth/protocols/eth"
	"github.com/ethereum/go-ethereum/p2p"
)

// Fetch availability bytes while preserving the connection for subsequent
// SNAP queries. The exact supplied hash is checked; consensus authority still
// comes only from native Gloas verification and its same-process capability.
func witnessExecutionHeader(ctx context.Context, rw p2p.MsgReadWriter, want common.Hash, version uint) ([]byte, error) {
	id, err := witnessRequestID()
	if err != nil {
		return nil, err
	}
	request := eth.GetBlockHeadersPacket{RequestId: id, GetBlockHeadersRequest: &eth.GetBlockHeadersRequest{Origin: eth.HashOrNumber{Hash: want}, Amount: 1}}
	if err = p2p.Send(rw, eth.GetBlockHeadersMsg, &request); err != nil {
		return nil, err
	}
	for {
		if err = ctx.Err(); err != nil {
			return nil, err
		}
		m, err := rw.ReadMsg()
		if err != nil {
			return nil, err
		}
		switch m.Code {
		case eth.BlockHeadersMsg:
			var packet eth.BlockHeadersRLPPacket
			if err = m.Decode(&packet); err != nil {
				return nil, err
			}
			return acceptHeader(packet, id, want)
		case eth.GetBlockHeadersMsg:
			var q eth.GetBlockHeadersPacket
			if err = m.Decode(&q); err != nil {
				return nil, err
			}
			if err = p2p.Send(rw, eth.BlockHeadersMsg, &eth.BlockHeadersRLPPacket{RequestId: q.RequestId}); err != nil {
				return nil, err
			}
		case eth.GetBlockBodiesMsg:
			var q eth.GetBlockBodiesPacket
			if err = m.Decode(&q); err != nil {
				return nil, err
			}
			if err = p2p.Send(rw, eth.BlockBodiesMsg, &eth.BlockBodiesRLPPacket{RequestId: q.RequestId}); err != nil {
				return nil, err
			}
		case eth.NewPooledTransactionHashesMsg, eth.TransactionsMsg, eth.BlockRangeUpdateMsg:
			if err = m.Discard(); err != nil {
				return nil, err
			}
		case eth.GetReceiptsMsg, eth.GetBlockAccessListsMsg, eth.GetCellsMsg:
			if _, err = replyUnavailableETHData(m, rw, version); err != nil {
				return nil, err
			}
		default:
			return nil, errors.New("unexpected ETH header availability message")
		}
	}
}
