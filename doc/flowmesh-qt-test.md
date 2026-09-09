# FlowMesh Qt test wallet

This is a development build, not a release. It uses the existing FlowMesh
protocol; it does not change FN membership, quorum, custody or finality rules.
The initial tUSD is **unbacked test money**, not a redeemable dollar or USDT.

Open **Trade** with a spending wallet selected. The workspace reads actual
markets and certified account balances from the local node. Assets → Deposit
to FlowMesh / Withdraw from FlowMesh opens that workspace for the selected
asset. These navigation buttons do not move funds.

## Units and review

- Colored-asset amounts are integer **atomic units**, not display amounts.
  For the existing six-decimal tUSD, `1000000` means **1 tUSD**.
- Native amounts are decimal **B3**, with at most nine decimal places.
- Limit prices are integer **B3 atoms per one asset atomic unit**. One B3 is
  1,000,000,000 B3 atoms. For six-decimal tUSD, a price of `1000` means
  **1 B3 per displayed tUSD**. A quantity of `1000000` at that price has a
  notional of **1 B3**. Check both fields carefully.
- Inspect the full market and asset IDs, amount, payout destination and fee
  in the confirmation. The tUSD is unbacked test money, but B3 transaction
  fees are real and deposits place funds in keyless custody.

## Deposit, trade and withdraw are separate stages

1. Select a running, unpaused market with eligible FN seats and a working
   signing quorum. Merely reaching the activation height does not supply one.
2. Prepare and review a deposit. Keep its transaction ID and output index.
   Wait for **31 confirmations** (creation block 30 blocks behind the tip),
   then submit that output for admission to FlowMesh.
3. FN validators must certify the action. A certified checkpoint must be
   published on B3, followed by the connected deposit-sweep transaction.
   Only the resulting certified account balance is available for trading.
4. Submit a limit bid or ask. Acceptance is a queued action, not a fill.
   Counterparty liquidity and FN progress are required. Do not repeatedly
   submit the same action after an uncertain result; inspect current status.
5. A withdrawal request is not a payment. After certification and checkpoint
   connection, its certified withdrawal effect can be published to the exact
   destination committed by the request. Verify the resulting B3 transaction
   and wallet receipt before treating the withdrawal as complete.

Publish same-market/asset vault effects one at a time. Wait for each B3
transaction to confirm, then refresh: pool inputs may overlap.

If the market is paused or signing stops, do not add more deposits. There is
no timeout that lets an individual wallet bypass the FN custody quorum.
Never delete signer journals to force a transaction through.

Back up the wallet before testing. Preparation can create account/change keys
even if a transaction is cancelled. Preserve all signer journals. Close the
old application cleanly before opening another build on the same data folder;
do not run duplicate instances with the same signing keys.
