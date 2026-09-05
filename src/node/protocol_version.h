// Copyright (c) 2012-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_PROTOCOL_VERSION_H
#define BITCOIN_NODE_PROTOCOL_VERSION_H

/**
 * network protocol versioning
 */

static const int PROTOCOL_VERSION = 70016;

/**
 * B3's post-transition wire identity. This is intentionally separate from
 * PROTOCOL_VERSION: modern B3 peers advertise a monotone successor to the
 * historical 80008 banner, while Core feature negotiation remains capped at
 * PROTOCOL_VERSION below. 80009 was the first modern identity; 80010 is the
 * v1.1.3 finality-recovery build. Every value above 80008 negotiates the
 * same modern mode, so the bump identifies builds without changing
 * compatibility.
 */
static const int B3_MODERN_PROTOCOL_VERSION = 80010;

//! initial proto version, to be increased after version/verack negotiation
static const int INIT_PROTO_VERSION = 209;

//! disconnect from peers older than this proto version
static const int MIN_PEER_PROTO_VERSION = 31800;

//! BIP 0031, pong message, is enabled for all versions AFTER this one
static const int BIP0031_VERSION = 60000;

//! "sendheaders" message type and announcing blocks with headers starts with this version
static const int SENDHEADERS_VERSION = 70012;

//! "feefilter" tells peers to filter invs to you by fee starts with this version
static const int FEEFILTER_VERSION = 70013;

//! short-id-based block download starts with this version
static const int SHORT_IDS_BLOCKS_VERSION = 70014;

//! not banning for invalid compact blocks starts with this version
static const int INVALID_CB_NO_BAN_VERSION = 70015;

//! "wtxidrelay" message type for wtxid-based relay starts with this version
static const int WTXID_RELAY_VERSION = 70016;

#endif // BITCOIN_NODE_PROTOCOL_VERSION_H
