// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {TestBase} from "./TestBase.sol";
import {B3BridgeAssetId} from "../src/B3BridgeAssetId.sol";

contract BridgeAssetIdVectorHarness {
    function compute(bytes32 chainDomain, uint64 originChainId, address vault, address token)
        external
        pure
        returns (bytes32)
    {
        return B3BridgeAssetId.compute(chainDomain, originChainId, vault, token);
    }
}

contract BridgeAssetIdVectorTest is TestBase {
    // This non-symmetric fixture is also asserted by
    // src/test/bridge_asset_vector_tests.cpp. It catches both uint64 endianness
    // and accidental use of B3's reverse display hex as EVM bytes32 input.
    function test_CppSolidityAssetIdByteOrderVector() public {
        BridgeAssetIdVectorHarness harness = new BridgeAssetIdVectorHarness();
        bytes32 chainDomain = hex"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
        bytes32 expected = hex"72eca63ac9f140eee09f06cf86faed49cbe961b8b07e0dd291f76af30dc826d3";

        assertEq(
            harness.compute(
                chainDomain, 1, 0x11223344556677889900aABbCcdDEeFF00112233, 0xdAC17F958D2ee523a2206206994597C13D831ec7
            ),
            expected
        );
    }
}
