"""Synthetic decimal/atom and fee-floor cases with independent expected values.

No RPC, network, wallet, private key, floating-point money or production imports.
The zero AssetId models native B3 identity; amounts remain exact integer atoms.
"""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from model import Model, action, action_id, load_profile


def identifier(number):
    return number.to_bytes(32, "big").hex()


NATIVE_B3 = "0" * 64
TOKEN = "c" * 64
QUOTE_A = "a" * 64
QUOTE_B = "b" * 64
BUYER, SELLER = identifier(1), identifier(2)
SEAT_LOW, SEAT_HIGH = identifier(0x100), identifier(0x10000)


class DecimalAndFeeTests(unittest.TestCase):
    def make_model(self, markets, native_decimals=9, quote_a_decimals=6, quote_b_decimals=0):
        model = Model(
            load_profile(),
            {NATIVE_B3: {"decimals": native_decimals, "symbol": "B3"},
             TOKEN: {"decimals": 8, "symbol": "TOKEN"},
             QUOTE_A: {"decimals": quote_a_decimals, "symbol": "USD"},
             QUOTE_B: {"decimals": quote_b_decimals, "symbol": "USD"}},
            markets,
            {SEAT_HIGH: identifier(11), SEAT_LOW: identifier(10)},
            identifier(12), {},
        )
        return model

    def market(self, base, quote, base_atoms_per_lot=1, quote_atoms_per_tick=1):
        return {"base": base, "quote": quote,
                "base_atoms_per_lot": base_atoms_per_lot,
                "quote_atoms_per_tick": quote_atoms_per_tick}

    def fund(self, model, facts):
        model.apply_batch(deposits=[{
            "chain": "generated-decimal-fixture", "custody_version": 2,
            "custody_domain": identifier(20), "txid": identifier(100 + index),
            "vout": 0, "account": account, "asset": asset, "amount": amount,
            "height": 1, "tx_index": index, "output_index": 0, "verified": True,
        } for index, (account, asset, amount) in enumerate(facts)])

    def open(self, account, sequence, side, market, price, lots):
        points = [[price, lots], [price + 1, 0]] if side == "BUY" else [[price - 1, 0], [price, lots]]
        return action(account, sequence, "OPEN", market=market, side=side, curve=points)

    def execute(self, model, instructions):
        report = model.apply_batch(actions=instructions)
        for instruction in instructions:
            self.assertEqual(report["outcomes"][action_id(instruction)]["status"], "ACCEPTED")
        model.assert_invariants()
        return report

    def balance(self, model, account, asset):
        return model.state["spot"].get(account, {}).get(asset, 0)

    def fees(self, model, asset):
        return model.state["fees_collected"].get(asset, 0)

    def treasury(self, model, asset):
        return model.state["treasury"].get(asset, 0)

    def fn(self, model, seat, asset):
        return model.state["fn"].get(seat, {}).get(asset, 0)

    def test_native_zero_asset_id_nine_six_decimals_trade_explicit_atoms(self):
        # Lot = .01 B3; tick = .025 USD. At four ticks, three lots move
        # .03 B3 and .3 USD: 30,000,000 base atoms and 300,000 quote atoms.
        # Each side owes 15 quote atoms (300,000 * 50 / 1,000,000).
        model = self.make_model([self.market(NATIVE_B3, QUOTE_A, 10_000_000, 25_000)])
        self.fund(model, [(BUYER, QUOTE_A, 1_000_000), (SELLER, NATIVE_B3, 30_000_000)])
        market = model.market_ids[0]
        report = self.execute(model, [self.open(BUYER, 0, "BUY", market, 4, 3),
                                      self.open(SELLER, 0, "SELL", market, 4, 3)])
        self.assertEqual((report["fills"][0]["price"], report["fills"][0]["volume"]), (4, 3))
        self.assertEqual(self.balance(model, BUYER, NATIVE_B3), 30_000_000)
        self.assertEqual(self.balance(model, SELLER, NATIVE_B3), 0)
        self.assertEqual(self.balance(model, BUYER, QUOTE_A), 699_985)
        self.assertEqual(self.balance(model, SELLER, QUOTE_A), 299_985)
        self.assertEqual(self.fees(model, QUOTE_A), 30)
        self.assertEqual(self.treasury(model, QUOTE_A), 6)
        self.assertEqual((self.fn(model, SEAT_LOW, QUOTE_A), self.fn(model, SEAT_HIGH, QUOTE_A)), (12, 12))
        self.assertEqual(model.state["custody"][NATIVE_B3], 30_000_000)
        self.assertEqual(self.fees(model, NATIVE_B3), 0)

    def test_eight_zero_decimals_trade_does_not_invent_fractional_quote_atoms(self):
        # Lot = .125 TOKEN; the zero-decimal quote has one whole unit per
        # atom. Two lots at 20,001 ticks transfer .25 TOKEN and 40,002 USD.
        # Each side's floor is two WHOLE quote units, with no fractional atom.
        model = self.make_model([self.market(TOKEN, QUOTE_B, 12_500_000, 1)])
        self.fund(model, [(BUYER, QUOTE_B, 50_000), (SELLER, TOKEN, 25_000_000)])
        market = model.market_ids[0]
        self.execute(model, [self.open(BUYER, 0, "BUY", market, 20_001, 2),
                             self.open(SELLER, 0, "SELL", market, 20_001, 2)])
        self.assertEqual(self.balance(model, BUYER, TOKEN), 25_000_000)
        self.assertEqual(self.balance(model, SELLER, TOKEN), 0)
        self.assertEqual(self.balance(model, BUYER, QUOTE_B), 9_996)
        self.assertEqual(self.balance(model, SELLER, QUOTE_B), 40_000)
        self.assertEqual(self.fees(model, QUOTE_B), 4)
        self.assertEqual(self.treasury(model, QUOTE_B), 0)
        self.assertEqual((self.fn(model, SEAT_LOW, QUOTE_B), self.fn(model, SEAT_HIGH, QUOTE_B)), (2, 2))
        self.assertEqual(self.balance(model, BUYER, QUOTE_A), 0)

    def test_fresh_tiny_orders_avoid_dust_that_cumulative_partial_fills_accrue(self):
        # Three fresh 9,999-atom orders pay floor(.49995)=0 each. A single
        # order cumulatively filled for 29,997 atoms pays floor(1.49985)=1.
        # This intentionally records the chosen profile's order-splitting
        # incentive; cumulative floors remove fill-splitting dust only.
        models = {}
        for standing_side in (None, "BUY", "SELL"):
            model = self.make_model([self.market(TOKEN, QUOTE_A)])
            self.fund(model, [(BUYER, QUOTE_A, 100_000), (SELLER, TOKEN, 3)])
            market = model.market_ids[0]
            if standing_side is not None:
                account = BUYER if standing_side == "BUY" else SELLER
                self.execute(model, [self.open(account, 0, standing_side, market, 9_999, 3)])
            for sequence in range(3):
                instructions = []
                if standing_side != "BUY":
                    instructions.append(self.open(BUYER, sequence, "BUY", market, 9_999, 1))
                if standing_side != "SELL":
                    instructions.append(self.open(SELLER, sequence, "SELL", market, 9_999, 1))
                self.execute(model, instructions)
            self.assertEqual(self.balance(model, BUYER, TOKEN), 3)
            self.assertEqual(self.balance(model, SELLER, TOKEN), 0)
            models[standing_side] = model

        fresh, cumulative_buy, cumulative_sell = models[None], models["BUY"], models["SELL"]
        self.assertEqual((self.fees(fresh, QUOTE_A), self.fees(cumulative_buy, QUOTE_A),
                          self.fees(cumulative_sell, QUOTE_A)), (0, 1, 1))
        self.assertEqual(self.balance(fresh, BUYER, QUOTE_A), 70_003)
        self.assertEqual(self.balance(fresh, SELLER, QUOTE_A), 29_997)
        self.assertEqual(self.balance(cumulative_buy, BUYER, QUOTE_A), 70_002)
        self.assertEqual(self.balance(cumulative_buy, SELLER, QUOTE_A), 29_997)
        self.assertEqual(self.balance(cumulative_sell, BUYER, QUOTE_A), 70_003)
        self.assertEqual(self.balance(cumulative_sell, SELLER, QUOTE_A), 29_996)
        for model in (cumulative_buy, cumulative_sell):
            self.assertEqual(self.treasury(model, QUOTE_A), 0)
            self.assertEqual((self.fn(model, SEAT_LOW, QUOTE_A), self.fn(model, SEAT_HIGH, QUOTE_A)), (1, 0))

    def two_market_trades(self, separate_quote_ids=False, separate_batches=False):
        second_quote = QUOTE_B if separate_quote_ids else QUOTE_A
        model = self.make_model([self.market(NATIVE_B3, QUOTE_A), self.market(TOKEN, second_quote)],
                                quote_b_decimals=6)
        deposits = [(BUYER, QUOTE_A, 100_000), (SELLER, NATIVE_B3, 1), (SELLER, TOKEN, 1)]
        if separate_quote_ids:
            deposits.append((BUYER, QUOTE_B, 100_000))
        self.fund(model, deposits)
        batches = []
        for sequence, market in enumerate(model.market_ids):
            batches.append([self.open(BUYER, sequence, "BUY", market, 40_000, 1),
                            self.open(SELLER, sequence, "SELL", market, 40_000, 1)])
        if separate_batches:
            for batch in batches:
                self.execute(model, batch)
        else:
            self.execute(model, batches[0] + batches[1])
        self.assertEqual(self.balance(model, BUYER, NATIVE_B3), 1)
        self.assertEqual(self.balance(model, BUYER, TOKEN), 1)
        return model

    def test_same_batch_shared_exact_quote_combines_market_fees_before_treasury_floor(self):
        # Each market collects two atoms from each side: four per market.
        # Shared exact quote: floor((4 + 4) * 20%) = 1 treasury atom.
        model = self.two_market_trades()
        self.assertEqual(self.fees(model, QUOTE_A), 8)
        self.assertEqual(self.treasury(model, QUOTE_A), 1)
        self.assertEqual((self.fn(model, SEAT_LOW, QUOTE_A), self.fn(model, SEAT_HIGH, QUOTE_A)), (4, 3))
        self.assertEqual(self.balance(model, BUYER, QUOTE_A), 19_996)
        self.assertEqual(self.balance(model, SELLER, QUOTE_A), 79_996)

    def test_same_symbol_separate_quote_ids_floor_independently(self):
        # Both exact quote assets deliberately have the same USD symbol and
        # six decimals. They still yield floor(4 * 20%) = 0 treasury each.
        model = self.two_market_trades(separate_quote_ids=True)
        for quote in (QUOTE_A, QUOTE_B):
            self.assertEqual(self.fees(model, quote), 4)
            self.assertEqual(self.treasury(model, quote), 0)
            self.assertEqual((self.fn(model, SEAT_LOW, quote), self.fn(model, SEAT_HIGH, quote)), (2, 2))
            self.assertEqual(self.balance(model, BUYER, quote), 59_998)
            self.assertEqual(self.balance(model, SELLER, quote), 39_998)

    def test_shared_quote_separate_batches_do_not_carry_treasury_dust(self):
        model = self.two_market_trades(separate_batches=True)
        self.assertEqual(self.fees(model, QUOTE_A), 8)
        self.assertEqual(self.treasury(model, QUOTE_A), 0)
        self.assertEqual((self.fn(model, SEAT_LOW, QUOTE_A), self.fn(model, SEAT_HIGH, QUOTE_A)), (4, 4))
        self.assertEqual(self.balance(model, BUYER, QUOTE_A), 19_996)
        self.assertEqual(self.balance(model, SELLER, QUOTE_A), 79_996)


if __name__ == "__main__":
    unittest.main()
