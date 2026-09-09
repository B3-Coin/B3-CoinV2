# Using the FlowMesh Trade page

Select your wallet, open **Trade**, and choose an established token/B3 market.
The page does not create a market or start FlowMesh automatically: its runtime
must already be running. Trading needs a participating validator quorum; an
available runtime or a displayed seat threshold does not prove validators are
online. Ordinary traders do not need to arm FN validator keys.

## Fund your trading balance

1. Choose **Deposit → New deposit**, select the token or B3, and enter an amount
   in displayed token units. Review the wallet, full asset ID, amount and actual
   B3 network fee before confirming the exact transaction.
2. Wait for **at least 31 B3 confirmations**. Keep the deposit transaction ID
   and its output index. Back up the wallet: preparation may create its trading
   account key even if you cancel before broadcasting.
3. Choose **Deposit → Use existing deposit** and enter that transaction ID and
   output index. This requests admission of the already funded output; it does
   not send another deposit. Wait for certification and the credited balance.
4. Admission is not finished on-chain settlement. An operator must publish the
   certified checkpoint and then the connected deposit sweep, which moves the
   existing deposit into pool custody. These are separate fee-funded actions
   under **Advanced settlement and market details**.

Deposits enter a keyless vault and may remain locked if quorum or settlement
fails. Keep ordinary wallet B3 available for network fees; a FlowMesh balance
is not ordinary wallet spending balance. Do not make a second deposit to fix a
pending first deposit.

## Place and follow an order

Enter quantity in tokens and price in **B3 per token**, then choose Buy or Sell
and review. For a verified six-decimal tUSD asset, quantity `1` means one tUSD;
price `1` means one B3 per tUSD. Inputs must fit the displayed steps: the page
rejects off-grid values instead of rounding them. The spot fee is 0.01% of
matched B3 notional, deducted from seller proceeds. An order request itself
has no on-chain network fee.

There is **one order per side per account**. A new Buy replaces that account's
Buy curve; a new Sell replaces its Sell curve. FlowMesh matches persistent
demand/supply curves at one uniform price per certified auction, not by
price-time order-book priority. **Your orders** shows certified remaining and
reserved amounts. Choose Buy or Sell in the order form, then use the matching
cancellation button under **Your orders**.

- **Accepted** means the request was queued, not executed or filled.
- **Certified** means a quorum certified the resulting state. An order or
  cancellation can be certified without a trade occurring.
- A **fill** is an actual matched quantity in **Certified trades**. Partial
  fills can leave an order and its reservation standing.

If an action is pending, stop submitting replacements or duplicates and inspect
**Activity**, the certified account sequence and balances. Cancellation also
needs certification; it is not instant. After an uncertain error, refresh and
inspect the saved action/transaction before using **Review uncertain
submission…**. That acknowledgement only enables new actions; it never retries
the old one or proves it failed.

## Read the graph and liquidity

Price points are actual certified clearings, indexed by **microblock sequence**,
not timestamps or invented candles. Liquidity shows demand and supply evaluated
at each price—not cumulative CLOB levels. Curve lines are a display guide, not
an executable quote. Partial curve pages and bounded recent history are labeled;
a dash in your fill column means unknown, not zero.

An idle market may produce no new microblocks. An old chart point alone does not
prove loss of quorum. Stale reads, paused markets and validator handoffs can
disable new trading; the page does not guarantee execution latency. Test-asset
labels and tickers such as tUSD do not establish dollar backing or redemption.

## Withdraw and settle

Choose **Withdraw**, the asset and amount, and a B3 destination address. This
submits a withdrawal request, **not an immediate payout**. Quorum must certify
it; a checkpoint must connect to B3; then an operator publishes its certified
vault payout to the same bound destination. Wait for on-chain confirmation.

The advanced pane offers **Prepare pending checkpoint…** and **Prepare selected
sweep / payout…**. Each prepares first and requires a separate exact-transaction
and B3-fee confirmation. It never spends settlement fees automatically. A
pending genesis checkpoint can be published while the market is paused; this
does not bypass quorum or proof requirements. Wait for confirmation before
publishing another overlapping vault effect.
