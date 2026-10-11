#!/usr/bin/env python3
"""Generated native stores only: malformed evidence must fence, never repair."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

from native_checker import decode
from run_recovery import World


def case(binary, mutator, fixture, root, mode):
    w = World(binary, fixture, root)
    result = {'case':mode,'status':'FAILED'}
    try:
        seat = 1
        if mode == 'duplicate-owner':
            p = subprocess.run([str(binary),'worker','0',str(root/'store0')],
                               capture_output=True,text=True,timeout=15)
            (root/'second-owner.stderr').write_text(p.stderr)
            assert p.returncode == 1 and 'lock' in p.stderr.lower(), p.stderr
            w.command(0, {'cmd':'status'})
            assert not w.statuses[0]['fenced']
            result['refused_child_exit'] = p.returncode
        else:
            if mode == 'drop-report1':
                seat = 2
                w.body(seat)
                w.command(seat, {'cmd':'tick','now':3})
                w.command(seat, {'cmd':'tick','now':6})
                w.time = 6
                assert w.statuses[seat]['view'] == 2 and w.statuses[seat]['changing']
            elif mode == 'nv-without-report1':
                for node in range(4):
                    w.body(node)
                # Drop the original proposal so no view-zero decision exists.
                w.pump(drop=lambda s,k,p:k == 'proof')
                w.tick()
                def no_self_report(target, kind, wire):
                    if kind != 'proof':
                        return False
                    p = decode(wire)
                    return p.view == 0 or (target == 1 and p.kind == 'REPORT' and p.sender == 1)
                w.pump(drop=no_self_report)
                w.settled(value=fixture['bodies'][0]['value'])
                nv = w.checker.issued[1,'NEW_VIEW',1]
                assert {r.sender for r in nv.items[1:]} == {0,2,3}
            else:
                if mode == 'drop-commit-highest':
                    w.kill(0)
                for node in (1,2,3):
                    w.body(node)
                if 0 in w.children:
                    w.body(0)
                else:
                    w.tick()
                w.pump()
                w.settled()
            w.stop(seat)
            shutil.copytree(root/f'store{seat}', root/'original-preserved-store')
            mutation = 'drop-report1' if mode == 'nv-without-report1' else mode
            altered = subprocess.run([str(mutator),'corrupt-test-store',str(seat),
                                      str(root/f'store{seat}'),mutation],
                                     capture_output=True,text=True,timeout=20)
            (root/'mutation.stdout').write_text(altered.stdout)
            (root/'mutation.stderr').write_text(altered.stderr)
            assert altered.returncode == 0, altered.stderr
            w.start(seat)
            assert w.statuses[seat]['fenced'], 'incomplete retained evidence was not fenced'
            assert w.statuses[seat]['reason'].startswith('RESTART_SAFE_REFUSAL:')
            response = w.command(seat, {'cmd':'tick','now':w.time+1}, allow_rejection=True)
            assert not response['ok'] and not response['events']
            result['refusal'] = w.statuses[seat]['reason']
        w.close()
        result['status'] = 'PASS'
    finally:
        w.cleanup()
        result['exits'] = w.exits
        (root/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary',type=Path,required=True)
    p.add_argument('--mutator',type=Path)
    p.add_argument('--evidence',type=Path,required=True)
    p.add_argument('--case',choices=['all','drop-report1','nv-without-report1',
                                   'drop-v0','drop-commit-highest','bad-snapshot','duplicate-owner'],default='all')
    a = p.parse_args()
    binary = a.binary.resolve(strict=True)
    mutator = a.mutator.resolve(strict=True) if a.mutator else binary
    a.evidence.mkdir(parents=True,exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='store-refusal-',dir=a.evidence))
    fixture = json.loads(subprocess.run([str(binary),'generate'],capture_output=True,
                                       text=True,check=True,timeout=30).stdout)
    cases = ['drop-report1','nv-without-report1','drop-v0','drop-commit-highest','bad-snapshot','duplicate-owner'] if a.case == 'all' else [a.case]
    print(json.dumps({'evidence':str(root)}),flush=True)
    for mode in cases:
        path = root/mode
        path.mkdir()
        print(json.dumps(case(binary,mutator,fixture,path,mode)),flush=True)


if __name__ == '__main__':
    main()
