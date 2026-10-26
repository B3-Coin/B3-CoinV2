// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license.
package main

import "github.com/ethereum/go-ethereum/p2p/enode"

// Public bootnodes seed discovery; they are not promised execution servers.
// Geth prioritizes static dials ahead of discovered candidates. Pinning every
// bootnode there repeatedly spends the small peer budget on busy seed nodes.
// Only the explicitly selected owned fixture is a static endpoint.
func fixtureStaticNodes(fixture string, nodes []*enode.Node) []*enode.Node {
	if fixture == "" {
		return nil
	}
	return nodes
}
