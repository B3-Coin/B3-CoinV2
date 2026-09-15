# Short closed-tester checklist

All tokens are unbacked/valueless regtest assets. Do not use a real wallet.

1. Verify CANDIDATE.json, archive checksum, platform and the explicitly approved
   local environment. Stopped services are not reachable endpoints.
2. Launch the extracted candidate. Create your own fresh `closed-test` wallet.
   Confirm `[regtest]`, correct wallet identity and validator engine OFF.
3. Give the coordinator only a new public test address for valueless funding.
4. Open Trade; inspect the exact market and AssetId. The canonical market is
   colored-asset/B3; the UI is a B3-facing reciprocal view, not a new market.
5. Deposit a reviewed small test amount. Wait for certified credit, not merely
   transaction submission or endpoint admission.
6. Review a new Buy/Sell B3 instruction: correct market, canonical side, price,
   token quantity cap, B3 reservation, fee currency and account sequence. Token
   caps are not a guarantee of an exact B3 fill. Actual V1 trading fees are B3.
7. Submit once and observe certified inclusion/result evidence separately from
   admission. A standing order is not a fill. Then review and submit one
   separate cancellation and verify its certified release.
8. Select the two saved requests with ordinary mouse/keyboard. Confirm the
   visible ActionId/sequence/card really changes. If automation cannot operate
   the selector, do this single manual check; do not repeat ineffective probes.
9. Check status during background refresh. Select the other request before the
   result arrives and verify the old result cannot overwrite its card. Report
   the exact identity/timestamps; do not submit another action for this check.
10. Quit using the native application menu or Cmd+Q. Record actual child exit
    status and new Shutdown done. Reopen the SAME wallet/profile. Confirm
    original signed bytes/ActionIds/sequences and certified/no-resubmit records.
    An uncertain instruction may retry identical bytes under existing rules;
    never re-sign a replacement just to make the screen appear successful.

Optional advanced tests are separate, using generated fixtures only:

- STAKE: inspect the marked owner-spendable selection/warning, then switch to
  the generated watch-only wallet. It must not inherit owner authority/readiness.
  Return to the owner and verify identity/unavailable-selection handling. No
  new spend is needed merely to repeat previously retained qualification.
- Migration: follow doc/b3-wallet-migration.md with generated legacy fixtures
  or separately approved disposable copies. Stop legacy cleanly and preserve
  complete original wallet/BDB environment/logs. Never migrate your only copy.
- Reconciliation: current UTXOs are not complete history. INCONCLUSIVE is not
  repair; matching totals do not prove recovery. Detailed ownership reports
  stay private; timeout may leave a server scan running, and abort must not
  cancel another user's scan.

Migration interruption tests are not power-loss or Windows durability tests.
Inspect KNOWN-ISSUES.md. Report privately using PRIVATE-REPORT.md; never attach
keys, passwords, wallets, RPC cookies or signer journal contents.
