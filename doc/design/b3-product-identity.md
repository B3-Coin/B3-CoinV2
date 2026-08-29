# B3 product identity — LOCKED (owner rulings 2026-08-25 and 2026-08-29)

| Thing | Name |
|---|---|
| Platform / network / ecosystem | **B3 FlowMesh** |
| Desktop application | **B3 Hive** |
| Native asset | **B3** |
| Legacy chain / history / compatibility-sensitive internal identifiers | B3Coin (only) |

**B3 FlowMesh is the complete modern system** — base-chain settlement, Modern
PoS, microblocks, assets, FN participation, trading and bridge capabilities.
It must not be described as merely the DEX engine. (Documents predating this
ruling that use "FlowMesh" for the DEX subsystem are historical; new writing
follows this ruling.)

The application may display: `B3 HIVE`, `B3 FLOWMESH CLIENT`.

## Implementation mapping

- Visible "B3Coin Core" branding is removed:
  - CMake `CLIENT_NAME` = `B3 FlowMesh client` (daemon banner, lock/error
    messages, `-version` output, copyright line = "The B3 FlowMesh developers").
  - Qt displays `B3 Hive` (`HIVE_NAME` in `src/qt/guiconstants.h`): window
    title, About, tray, splash, overlays, dialogs.
  - **2026-08-29 owner supersession:** the BIP14 P2P user agent is
    `/B3Hive:<version>/` (`UA_NAME = B3Hive`), matching the product identity
    without changing the P2P message format or consensus behavior. External
    monitoring that filters the old identifier must allow the new one.
- **Preserved for compatibility (internal, not presented as product):**
  - `b3coin-*` binary names, the `b3coin:` URI scheme, datadir names,
    `QAPP_*` QSettings keys (`B3Coin`, `B3Coin-Core*`), source-file copyright
    headers, and network magic/ports.
  - These migrate, if ever, as separate owner-ruled compatibility work.
