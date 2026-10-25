#!/usr/bin/env python3
"""Tool and build locations of the regtest gates.

A path comes from its command-line argument, else from its environment
variable, else the program stops with a message that names both. There is no
machine-specific default. The argument names are unchanged, so an invocation
that passes every path explicitly behaves as before.

  FM2_TOOLS          Foundry 1.8.5 directory (forge, anvil, cast)   --tools
  FM2_SOLC           solc 0.8.35 binary                             --solc
  FM2_B3_NODE_BUILD  regtest b3coind build directory                --b3-node-build,
                     and bin/b3coind, bin/b3coin-cli inside it      --b3coind, --b3coin-cli
  FM2_REGTEST_BUILD  fm2 build directory; bin/fm2-regtest-node      --worker
  FM2_ASAN_BUILD     ASan+UBSan fm2 build directory                 --asan-build
  FM2_TSAN_BUILD     TSan fm2 build directory                       --tsan-build

contrib/flowmesh2/regtest/setup_tools.sh fetches and verifies FM2_TOOLS and
FM2_SOLC.
"""

import os
from pathlib import Path

# attribute name -> (flag, environment variable, path inside the variable's directory or None)
PATHS = {
    'tools': ('--tools', 'FM2_TOOLS', None),
    'solc': ('--solc', 'FM2_SOLC', None),
    'b3_node_build': ('--b3-node-build', 'FM2_B3_NODE_BUILD', None),
    'b3coind': ('--b3coind', 'FM2_B3_NODE_BUILD', 'bin/b3coind'),
    'b3coin_cli': ('--b3coin-cli', 'FM2_B3_NODE_BUILD', 'bin/b3coin-cli'),
    'worker': ('--worker', 'FM2_REGTEST_BUILD', 'bin/fm2-regtest-node'),
    'asan_build': ('--asan-build', 'FM2_ASAN_BUILD', None),
    'tsan_build': ('--tsan-build', 'FM2_TSAN_BUILD', None),
}


def add(parser, *names):
    """Add the named path arguments without a default."""
    for name in names:
        flag, variable, inside = PATHS[name]
        where = variable if inside is None else variable + '/' + inside
        parser.add_argument(flag, default=None, help='default: $' + where)


def lookup(name, value, environ=None):
    """Return the argument value, else the environment value; None if neither is set."""
    if value is not None:
        return value
    _, variable, inside = PATHS[name]
    base = (os.environ if environ is None else environ).get(variable, '')
    if not base:
        return None
    return base if inside is None else str(Path(base) / inside)


def resolve(parser, args, *names, environ=None):
    """Fill every named path of args, or stop through parser.error naming the flag and variable."""
    for name in names:
        value = lookup(name, getattr(args, name), environ)
        if value is None:
            flag, variable, _ = PATHS[name]
            parser.error(flag + ' is required: pass it or set ' + variable)
        setattr(args, name, value)
    return args
