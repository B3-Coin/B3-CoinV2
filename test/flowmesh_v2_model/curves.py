"""TEST ONLY: integer reference for the existing V1 curve auction.

Selection, floor interpolation and largest-remainder rationing mirror
src/flowmesh/clearing.h. This module does no settlement or authentication.
The caller checks configured asset bounds and can enforce an intermediate
ceiling with max_intermediate; omitted ceilings retain unbounded reference
arithmetic. Identifiers encode canonical bytes directly;
unlike uint256::GetHex(), this model's hex spelling does not reverse bytes.
"""

import re


HARD_MAX_CURVE_POINTS = 64
_IDENTIFIER = re.compile(r"[0-9a-f]{64}\Z")


def _integer(value, name, minimum=0):
    if type(value) is not int or value < minimum:
        raise ValueError(f"{name} must be an integer >= {minimum}")


def _validate_intermediate_limit(max_intermediate):
    if max_intermediate is not None:
        _integer(max_intermediate, "max_intermediate")


def _checked_intermediate(value, max_intermediate, name):
    if max_intermediate is not None and abs(value) > max_intermediate:
        raise ValueError(f"{name} exceeds intermediate bound")
    return value


def _check_curve(side, points, max_price=None, max_lots=None,
                 max_points=HARD_MAX_CURVE_POINTS):
    if side not in ("BUY", "SELL"):
        raise ValueError("side must be BUY or SELL")
    if not isinstance(points, list) or not 1 <= len(points) <= max_points:
        raise ValueError("curve point count outside bound")
    for index, point in enumerate(points):
        if not isinstance(point, list) or len(point) != 2:
            raise ValueError("curve point must be [price, lots]")
        price, lots = point
        _integer(price, "curve price")
        _integer(lots, "curve lots")
        if max_price is not None and price > max_price:
            raise ValueError("curve price exceeds bound")
        if max_lots is not None and lots > max_lots:
            raise ValueError("curve lots exceeds bound")
        if index:
            previous_price, previous_lots = points[index - 1]
            if price <= previous_price:
                raise ValueError("curve prices must strictly increase")
            if side == "BUY" and lots > previous_lots:
                raise ValueError("BUY quantities must not increase")
            if side == "SELL" and lots < previous_lots:
                raise ValueError("SELL quantities must not decrease")
    if side == "BUY" and (points[0][1] <= 0 or points[-1][1] != 0):
        raise ValueError("BUY must start positive and terminate at zero")
    if side == "SELL" and points[-1][1] <= 0:
        raise ValueError("SELL must reach positive quantity")


def validate_curve(side: str, points: list[list[int]], max_price: int,
                   max_lots: int, max_points: int) -> None:
    """Reject malformed, unbounded or nonmonotone curves with ValueError."""
    _integer(max_price, "max_price")
    _integer(max_lots, "max_lots", 1)
    _integer(max_points, "max_points", 1)
    if max_points > HARD_MAX_CURVE_POINTS:
        raise ValueError("max_points exceeds V1 hard point ceiling")
    _check_curve(side, points, max_price, max_lots, max_points)


def evaluate(points: list[list[int]], price: int, *, max_intermediate=None) -> int:
    """Evaluate a validated curve, flooring interpolation and clamping flat.

    Empty curves evaluate to zero as in V1. For nonempty curves the caller
    must validate first; V1's garbage-vector totality guards are not a new
    permissive admission rule in this accounting model. An optional ceiling
    bounds the absolute interpolation numerator, including BUY negatives.
    """
    _integer(price, "price")
    _validate_intermediate_limit(max_intermediate)
    if not points:
        return 0
    if price <= points[0][0]:
        return points[0][1]
    if price >= points[-1][0]:
        return points[-1][1]
    for index in range(1, len(points)):
        right_price, right_lots = points[index]
        if price < right_price:
            left_price, left_lots = points[index - 1]
            # Python // is floor, including for the negative BUY numerator.
            numerator = _checked_intermediate(
                (right_lots - left_lots) * (price - left_price),
                max_intermediate, "interpolation numerator")
            return left_lots + numerator // (right_price - left_price)
    return points[-1][1]


def buy_bound(points: list[list[int]], quote_atoms_per_tick: int, *,
              max_intermediate=None) -> int:
    """Conservative persistent staircase bound in exact quote atoms.

    A cumulative lot in (q[i+1], q[i]] cannot execute above p[i+1]-1.
    Summing those per-lot ceilings covers any sequence of partial fills.
    This is exact for [(P,Q),(P+1,0)], conservative for general curves;
    max_p(p * evaluate(points,p)) is not a safe persistent reservation.
    The caller applies monetary bounds to the result; max_intermediate, when
    supplied, bounds each product, running staircase sum and scaled result.
    """
    _integer(quote_atoms_per_tick, "quote_atoms_per_tick", 1)
    _validate_intermediate_limit(max_intermediate)
    _check_curve("BUY", points)
    total = 0
    for index in range(len(points) - 1):
        product = _checked_intermediate(
            (points[index][1] - points[index + 1][1]) *
            (points[index + 1][0] - 1), max_intermediate, "staircase product")
        total = _checked_intermediate(total + product, max_intermediate,
                                      "staircase sum")
    return _checked_intermediate(total * quote_atoms_per_tick, max_intermediate,
                                 "scaled staircase bound")


def auction(orders: list[dict], *, max_intermediate=None) -> dict:
    """Return one V1 uniform-price auction without mutating its orders.

    Orders must belong to a single market; the accounting model enforces
    its configured count, price, quantity and monetary bounds before calling.
    Candidate prices are ONLY the union of submitted curve breakpoints.
    Ties choose maximum volume, minimum exact imbalance, then lowest price.
    All supplied order IDs appear in fills, including zero allocations.
    The returned imbalance is exact (V1 caps only its reporting field).
    max_intermediate optionally bounds interpolation, demand/supply totals
    and rationing products; it rejects overflow without changing selection.
    """
    if not isinstance(orders, list):
        raise ValueError("orders must be a list")
    _validate_intermediate_limit(max_intermediate)
    seen_ids, seen_accounts, prices = set(), set(), set()
    for order in orders:
        if not isinstance(order, dict):
            raise ValueError("order must be a dict")
        for field in ("order_id", "account"):
            value = order.get(field)
            if not isinstance(value, str) or not _IDENTIFIER.fullmatch(value):
                raise ValueError(f"{field} must be canonical 64-lowercase-hex")
        side, points = order.get("side"), order.get("curve")
        _check_curve(side, points)
        filled = order.get("revision_filled")
        _integer(filled, "revision_filled")
        maximum = points[0][1] if side == "BUY" else points[-1][1]
        if filled > maximum:
            raise ValueError("revision_filled exceeds curve maximum")
        if order["order_id"] in seen_ids:
            raise ValueError("duplicate order_id")
        account_side = (order["account"], side)
        if account_side in seen_accounts:
            raise ValueError("duplicate account and side within market")
        seen_ids.add(order["order_id"])
        seen_accounts.add(account_side)
        prices.update(price for price, _ in points)

    fills = {order["order_id"]: 0 for order in
             sorted(orders, key=lambda order: bytes.fromhex(order["order_id"]))}
    result = {"price": None, "volume": 0, "fills": fills, "imbalance": 0}

    def effective(order, price):
        return max(0, evaluate(order["curve"], price,
                               max_intermediate=max_intermediate) -
                   order["revision_filled"])

    best_key = None
    for price in sorted(prices):
        demand = _checked_intermediate(
            sum(effective(order, price) for order in orders if order["side"] == "BUY"),
            max_intermediate, "aggregate demand")
        supply = _checked_intermediate(
            sum(effective(order, price) for order in orders if order["side"] == "SELL"),
            max_intermediate, "aggregate supply")
        volume, imbalance = min(demand, supply), abs(demand - supply)
        if volume <= 0:
            continue
        key = (-volume, imbalance, price)
        if best_key is None or key < best_key:
            best_key = key
            result.update(price=price, volume=volume, imbalance=imbalance)
    if result["price"] is None:
        return result

    for side in ("BUY", "SELL"):
        desired = [(order, effective(order, result["price"])) for order in orders
                   if order["side"] == side]
        desired = sorted(((order, qty) for order, qty in desired if qty > 0),
                         key=lambda item: bytes.fromhex(item[0]["account"]))
        total = _checked_intermediate(sum(qty for _, qty in desired),
                                      max_intermediate, "allocation side total")
        if total <= result["volume"]:
            for order, qty in desired:
                fills[order["order_id"]] = qty
        else:
            remainders = []
            handed = 0
            for order, qty in desired:
                product = _checked_intermediate(qty * result["volume"],
                                                max_intermediate, "rationing product")
                base, remainder = divmod(product, total)
                fills[order["order_id"]] = base
                handed += base
                remainders.append((order, remainder))
            # Stable sort preserves the canonical account order on ties.
            remainders.sort(key=lambda item: -item[1])
            for order, _ in remainders[:result["volume"] - handed]:
                fills[order["order_id"]] += 1
        if sum(fills[order["order_id"]] for order in orders
               if order["side"] == side) != result["volume"]:
            raise ValueError("auction allocation does not conserve volume")
    return result
