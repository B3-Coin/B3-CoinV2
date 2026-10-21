#!/usr/bin/env python3
"""Bounded native process/disk/crypto recovery scenarios, NOT latency or WAN.

The supervisor is a deterministic message scheduler over pipes. It cannot sign
for honest seats. Each native worker owns a different generated signer store.
Original commands, publications, guards, results and actual exits are retained.
"""
import argparse
from collections import deque
import hashlib
import json
from pathlib import Path
import queue
import random
import subprocess
import tempfile
import threading

from native_checker import Checker, Proof, decode, encode, empty


class Cut(Exception):
    pass


class Child:
    def __init__(self, binary, seat, root, attempt, byzantine=False, now=0):
        self.seat = seat
        self.attempt = attempt
        self.q = queue.Queue()
        self.err = (root/f"seat{seat}-start{attempt}.stderr").open('w')
        self.out = (root/f"seat{seat}-start{attempt}.jsonl").open('w')
        args = [str(binary), 'worker', str(seat), str(root/f'store{seat}')]
        args.append(f'--now={now}')
        if byzantine:
            args.append('--byzantine')
        self.p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=self.err, text=True, bufsize=1)
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def read(self):
        for line in self.p.stdout:
            self.out.write(line)
            self.out.flush()
            self.q.put(json.loads(line))
        self.q.put(None)

    def close_files(self):
        self.thread.join(timeout=2)
        self.out.close()
        self.err.close()


class World:
    def __init__(self, binary, fixture, root, bad=()):
        self.binary, self.fixture, self.root = binary, fixture, root
        self.bad = set(bad)
        self.checker = Checker(fixture['instance'], bad)
        self.children = {}
        self.history = []
        self.attempts = [0]*4
        self.queue = deque()
        self.statuses = {}
        self.time = 0
        self.steps = 0
        self.cuts = {}
        self.trace = (root/'schedule.jsonl').open('w')
        self.exits = []
        try:
            for seat in range(4):
                self.start(seat)
        except BaseException:
            self.cleanup()
            raise

    def start(self, seat):
        assert seat not in self.children
        self.attempts[seat] += 1
        child = Child(self.binary, seat, self.root, self.attempts[seat], seat in self.bad, self.time)
        self.children[seat] = child
        self.history.append(child)
        self.response(seat)

    def response(self, seat, data_target=None):
        child = self.children[seat]
        while True:
            response = child.q.get(timeout=60)
            if response is None:
                code = child.p.wait(timeout=5)
                self.exits.append({'seat': seat, 'attempt': self.attempts[seat], 'exit': code})
                self.children.pop(seat)
                child.close_files()
                assert code == 73 and seat in self.cuts, f'unexpected worker exit {seat}:{code}'
                self.cuts.pop(seat)
                raise Cut(seat)
            self.trace.write(json.dumps({'seat': seat, 'response': response})+'\n')
            self.trace.flush()
            for event in response.get('events', []):
                if 'issued' in event:
                    self.checker.issued_event(seat, event)
                if 'proof' in event:
                    proof = decode(event['proof'])
                    self.checker.proof(proof)
                    for target in range(4):
                        self.queue.append((target, 'proof', event['proof']))
                if 'need' in event:
                    for target in range(4):
                        if target != seat:
                            self.queue.append((target, 'need', (event['need'], seat)))
                if 'data' in event:
                    assert data_target is not None, 'unsolicited data routing'
                    self.queue.append((data_target, 'body', event['data']))
            if 'status' in response:
                self.statuses[seat] = response['status']
                self.checker.status(seat, response['status'])
            if response.get('partial'):
                continue
            return response

    def command(self, seat, command, data_target=None, allow_rejection=False):
        child = self.children[seat]
        self.trace.write(json.dumps({'seat': seat, 'command': command})+'\n')
        child.p.stdin.write(json.dumps(command)+'\n')
        child.p.stdin.flush()
        response = self.response(seat, data_target)
        assert response.get('ok', True) or allow_rejection, response
        return response

    def body(self, seat, index=0):
        return self.command(seat, {'cmd':'body', **self.fixture['bodies'][index]})

    def pump(self, drop=None, seed=None):
        rng = random.Random(seed)
        work = 0
        while self.queue:
            assert work < 5000 and len(self.queue) < 20000, 'bounded delivery exceeded'
            if seed is not None:
                self.queue.rotate(-rng.randrange(len(self.queue)))
            seat, kind, item = self.queue.popleft()
            work += 1
            if seat not in self.children or seat in self.bad or (drop and drop(seat,kind,item)):
                continue
            try:
                if kind == 'proof':
                    self.command(seat, {'cmd':'receive','proof':item}, allow_rejection=True)
                elif kind == 'body':
                    self.command(seat, {'cmd':'body',**item}, allow_rejection=True)
                elif kind == 'need':
                    value, requester = item
                    self.command(seat, {'cmd':'need','value':value}, data_target=requester)
            except Cut:
                pass
        self.steps += work

    def tick(self, step=3):
        self.time += step
        for seat in tuple(self.children):
            if seat not in self.bad:
                try:
                    self.command(seat, {'cmd':'tick','now':self.time})
                except Cut:
                    pass

    def kill(self, seat):
        child = self.children.pop(seat)
        child.p.kill()
        code = child.p.wait(timeout=5)
        assert code < 0
        child.close_files()
        self.exits.append({'seat':seat,'attempt':self.attempts[seat],'exit':code,'deliberate_kill':True})

    def byzantine_proposal(self, index, targets):
        value = self.fixture['bodies'][index]['value']
        vote = Proof('PREPARE',0,0,self.fixture['instance'],value,())
        self.command(0, {'cmd':'sign-byzantine','proof':encode(vote)})
        signed = next(p for p in self.checker.authenticated if p.kind == 'PREPARE' and p.sender == 0 and p.value == value)
        self.queue.clear()  # Explicit withheld Byzantine vote, observer retains it.
        proposal = Proof('PROPOSE',0,0,self.fixture['instance'],value,(signed,empty(self.fixture['instance'])))
        for seat in targets:
            self.queue.append((seat,'proof',encode(proposal)))

    def settled(self, seats=None, value=None):
        seats = set(self.children)-self.bad if seats is None else set(seats)
        for seat in seats:
            self.command(seat, {'cmd':'status'})
            state = self.statuses[seat]
            assert not state['fenced'] and state['apply_count'] == 1, state
            if value is not None:
                assert state['applied'] == value, state
        assert len({self.statuses[s]['state_root'] for s in seats}) == 1
        self.checker.hidden()

    def close(self):
        for seat in tuple(self.children):
            self.stop(seat)

    def stop(self, seat):
        child = self.children[seat]
        self.command(seat, {'cmd':'quit'})
        code = child.p.wait(timeout=10)
        assert code == 0
        child.close_files()
        self.exits.append({'seat':seat,'attempt':self.attempts[seat],'exit':code})
        self.children.pop(seat)

    def cleanup(self):
        for child in self.history:
            if child.p.poll() is None:
                child.p.terminate()
                try:
                    child.p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.p.kill()
                    child.p.wait(timeout=5)
            if not any(r['seat'] == child.seat and r['attempt'] == child.attempt for r in self.exits):
                self.exits.append({'seat':child.seat, 'attempt':child.attempt,
                                   'exit':child.p.returncode, 'failure_cleanup':True})
            if not child.out.closed:
                child.close_files()
        self.trace.close()


def execute(binary, fixture, root, scenario, cut=None, seed=None):
    w = World(binary, fixture, root, bad={0} if scenario == 'hidden-fast' else ())
    result = {'scenario':scenario,'cut':cut,'seed':seed,'status':'FAILED'}
    try:
        value = fixture['bodies'][0]['value']
        if scenario == 'future-pc':
            # Node0 is alive but receives neither NEW_VIEW1 nor its decision.
            # Its first PC1 arrives while it is still in view0.
            for seat in (1,2,3):
                w.body(seat)
                w.command(seat, {'cmd':'tick','now':3})
            w.time = 3
            w.pump(drop=lambda s,k,p:s == 0 and k == 'proof' and decode(p).kind != 'PC')
            assert w.statuses[0]['view'] == 0
            assert decode(w.statuses[0]['highest']).view == 1
            assert w.statuses[0]['apply_count'] == 0
            w.command(0, {'cmd':'tick','now':3})
            report = w.checker.issued[0,'REPORT',2]
            assert report.items[1].kind == 'PC' and report.items[1].view == 1
            assert (0,'REPORT',1) not in w.checker.issued
            w.tick(1)
            w.pump()
            w.settled(value=value)
        elif scenario == 'hidden-fast':
            for seat in (1,2,3):
                w.body(seat,0)
                w.body(seat,1)
            w.byzantine_proposal(0,[1,2])
            w.pump(drop=lambda s,k,p:k=='proof' and decode(p).kind in ('PREPARE','PC','FAST'))
            w.byzantine_proposal(1,[3])
            w.pump(drop=lambda s,k,p:k=='proof' and decode(p).kind in ('PREPARE','PC','FAST'))
            assert w.checker.decisions == {value}, 'hiddenFAST not reached'
            assert all(w.statuses[s]['apply_count'] == 0 for s in (1,2,3))
            w.tick()
            # Keep the original standalone votes/certificate withheld while
            # recovery uses the authenticated V0 evidence inside REPORT/NV.
            # Releasing PREPARE0 here would merely reconstruct old FAST and
            # would not exercise recovery's selection or COMMIT path.
            w.pump(drop=lambda s,k,p:k == 'proof' and decode(p).view == 0
                   and decode(p).kind in ('PREPARE','PC','FAST'), seed=seed)
            w.settled(value=value)
            assert all(decode(w.statuses[s]['decision']).kind == 'SLOW' for s in (1,2,3))
            votes = tuple(sorted((p for p in w.checker.authenticated if p.kind == 'PREPARE' and p.view == 0 and p.value == value), key=lambda p:p.sender))
            late = Proof('FAST',0,-1,fixture['instance'],value,votes)
            for seat in (1,2,3):
                w.command(seat,{'cmd':'receive','proof':encode(late)})
            w.pump()
            w.settled(value=value)
        else:
            if scenario in ('leader-failure','two-view-timeout'):
                w.kill(0)
            for seat in (1,2,3):
                if scenario != 'missing-body' or seat != 3:
                    w.body(seat)
            if cut:
                w.command(1, {'cmd':'set-cut','point':cut})
                w.cuts[1] = cut
            if scenario in ('serialize-failure','record-insert-failure'):
                fault = 'fail_signed_state_serialize' if scenario == 'serialize-failure' else 'fail_signed_record_insert'
                w.command(1, {'cmd':'set-cut','point':fault})
            if 0 in w.children:
                w.body(0)
            w.pump(seed=seed)
            if scenario in ('serialize-failure','record-insert-failure'):
                assert w.statuses[1]['fenced'], 'failed persistence did not fence publication'
                assert not any(k[0] == 1 for k in w.checker.issued), 'undurable signature escaped'
                assert not any(k in w.statuses[1] for k in ('v0','highest','decision')), 'failed-store status exposed proof-bearing mutable state'
                response = w.command(1, {'cmd':'tick','now':1}, allow_rejection=True)
                assert not response['ok'] and not response['events'], 'failed store resumed publication'
                w.stop(1)
                before_reopen = (root/'seat1-start1.jsonl').read_text()
                w.start(1)
                w.tick(1)
                w.pump()
                recovered = w.checker.issued[1,'PREPARE',0]
                assert recovered.signature.hex() not in before_reopen, 'undurable signature escaped outside events'
            if scenario in ('leader-failure','two-view-timeout'):
                w.tick()
                if scenario == 'two-view-timeout':
                    w.pump(drop=lambda s,k,p:k == 'proof')
                    w.tick()
                w.pump(seed=seed)
                w.settled(value=value)
                if scenario == 'two-view-timeout':
                    assert all(w.statuses[s]['view'] == 2 for s in (1,2,3))
                w.start(0)
                w.tick(1)
                w.pump()
            if cut:
                assert 1 not in w.children, 'cut path not reached'
                w.start(1)
                w.tick(1)
                w.pump()
            w.settled(value=value)
        # Reopen all honest workers from exact preserved stores, no body resubmission.
        before = {s: dict(w.statuses[s]) for s in w.children if s not in w.bad}
        w.close()
        for seat in before:
            w.start(seat)
        w.pump()
        w.settled(value=value)
        for seat, state in before.items():
            assert w.statuses[seat]['state_root'] == state['state_root']
        w.close()
        result.update(status='PASS', delivery_steps=w.steps, unique_honest_signatures=len(w.checker.issued),
                      retained_public_signatures=len(w.checker.authenticated), decisions=sorted(w.checker.decisions),
                      exits=w.exits, final_states=w.statuses)
    finally:
        w.cleanup()
        result['exits'] = w.exits
        (root/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary',type=Path,required=True)
    p.add_argument('--evidence',type=Path,required=True)
    p.add_argument('--scenario',default='healthy',choices=['healthy','leader-failure','hidden-fast','missing-body','cut','serialize-failure','record-insert-failure','future-pc','two-view-timeout'])
    p.add_argument('--cut',default='after_durable_before_publish')
    p.add_argument('--seed',type=int)
    a = p.parse_args()
    a.evidence.mkdir(parents=True,exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='recovery-',dir=a.evidence))
    binary = a.binary.resolve(strict=True)
    generated = subprocess.run([str(binary),'generate'],capture_output=True,text=True,timeout=30,check=True)
    fixture = json.loads(generated.stdout)
    (root/'fixture.json').write_text(json.dumps(fixture,indent=2)+'\n')
    (root/'binary.sha256').write_text(hashlib.sha256(binary.read_bytes()).hexdigest()+'\n')
    print(json.dumps({'evidence':str(root)}),flush=True)
    print(json.dumps(execute(binary,fixture,root,a.scenario,a.cut if a.scenario=='cut' else None,a.seed),indent=2))


if __name__ == '__main__':
    main()
