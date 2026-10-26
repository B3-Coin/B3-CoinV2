// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license.
package main

import (
	"github.com/ethereum/go-ethereum/p2p/enode"
	"testing"
)

func TestPublicBootnodesAreNotStaticServers(t *testing.T) {
	nodes := []*enode.Node{nil}
	if len(fixtureStaticNodes("", nodes)) != 0 {
		t.Fatal("public seeds pinned as execution servers")
	}
	if len(fixtureStaticNodes("owned-loopback", nodes)) != 1 {
		t.Fatal("owned fixture lost its explicit endpoint")
	}
}
