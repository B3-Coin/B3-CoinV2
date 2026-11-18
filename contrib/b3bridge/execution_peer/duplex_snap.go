// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license.
package main

import (
	"bytes"
	"context"
	"errors"
	"io"
	"sync"
	"sync/atomic"

	"github.com/ethereum/go-ethereum/p2p"
)

type witnessMessage struct{ message p2p.Msg }
type witnessDuplex struct {
	ctx      context.Context
	rw       p2p.MsgReadWriter
	incoming chan witnessMessage
	done     chan struct{}
	terminal error
	once     sync.Once
	pending  atomic.Bool
	queued   atomic.Bool
}

func (d *witnessDuplex) fail(err error) { d.once.Do(func() { d.terminal = err; close(d.done) }) }
func (d *witnessDuplex) Err() error {
	select {
	case <-d.done:
		return d.terminal
	default:
		return nil
	}
}

// Drain inbound requests even before ETH status is validated, serving only
// valid empty unavailable-data replies. Our queries remain ETH-gated.
func newWitnessDuplex(ctx context.Context, rw p2p.MsgReadWriter, version uint) *witnessDuplex {
	d := &witnessDuplex{ctx: ctx, rw: rw, incoming: make(chan witnessMessage, 1), done: make(chan struct{})}
	go func() {
		var total uint64
		for count := 0; count < 256; count++ {
			m, err := rw.ReadMsg()
			if err != nil {
				d.fail(err)
				return
			}
			if m.Size > maxSnapMessageBytes || total+uint64(m.Size) > maxWitnessBytes {
				d.fail(errors.New("SNAP duplex byte budget exceeded"))
				return
			}
			total += uint64(m.Size)
			raw, err := io.ReadAll(io.LimitReader(m.Payload, int64(m.Size)+1))
			if err != nil || len(raw) != int(m.Size) {
				d.fail(errors.New("incomplete SNAP message"))
				return
			}
			m.Payload = bytes.NewReader(raw)
			handled, err := replyUnavailableSnap(m, rw, version)
			if err != nil {
				d.fail(err)
				return
			}
			if handled {
				continue
			}
			// There is exactly one request in flight. Before the ETH gate no
			// response can be solicited; duplicate replies never fill a queue.
			if !d.pending.Load() || !d.queued.CompareAndSwap(false, true) {
				d.fail(errors.New("unsolicited or duplicate SNAP response"))
				return
			}
			select {
			case d.incoming <- witnessMessage{message: m}:
			case <-ctx.Done():
				d.fail(ctx.Err())
				return
			}
		}
		d.fail(errors.New("SNAP duplex message count budget exceeded"))
	}()
	return d
}
func (d *witnessDuplex) take(value witnessMessage) (p2p.Msg, error) {
	d.pending.Store(false)
	d.queued.Store(false)
	return value.message, nil
}
func (d *witnessDuplex) ReadMsg() (p2p.Msg, error) {
	// A complete bounded response already received remains available even if
	// the remote closes immediately after sending it.
	select {
	case value := <-d.incoming:
		return d.take(value)
	default:
	}
	select {
	case value := <-d.incoming:
		return d.take(value)
	case <-d.done:
		select {
		case value := <-d.incoming:
			return d.take(value)
		default:
			return p2p.Msg{}, d.terminal
		}
	case <-d.ctx.Done():
		return p2p.Msg{}, d.ctx.Err()
	}
}
func (d *witnessDuplex) WriteMsg(m p2p.Msg) error {
	if !d.pending.CompareAndSwap(false, true) {
		return errors.New("concurrent SNAP query refused")
	}
	if err := d.Err(); err != nil {
		d.pending.Store(false)
		return err
	}
	if err := d.rw.WriteMsg(m); err != nil {
		d.pending.Store(false)
		d.fail(err)
		return err
	}
	return nil
}
