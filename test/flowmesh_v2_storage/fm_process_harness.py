"""Generated local child-process fixture; NOT production IPC or authentication.

Only the parent owns the synthetic authentication audit, retained across child
deaths. Multiprocessing pipes carry trusted test objects, never external input.
Each child runs the unchanged Replica rules through the disk adapter. Parent
callbacks are acknowledged before the next child operation. Cuts use SIGKILL,
not a simulated restart or a Python exception masquerading as process death.
"""
from copy import deepcopy
import json
import multiprocessing
from pathlib import Path
import signal
import sys
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "flowmesh_v2_agreement"))
sys.path.insert(0, str(HERE))

from fm_application import Application
from fm_protocol import Proofs
from fm_simulator import Simulator

CALL_SECONDS = 20
PROCESS_RESULTS = []


def _view(node):
    names = ("d", "anchors", "alive", "deadline", "next_retry", "last_reason",
             "retry_work", "bodies", "offers", "local_offers", "references",
             "requests", "pending", "headers", "history_requests", "votes",
             "reports", "last_vote_cleanup_work")
    return {name: deepcopy(getattr(node, name)) for name in names}


def _worker(connection, index, n, snapshot, anchor, directory, create, trusted_head):
    # Imports in the child: no inherited open SQLite connection or signer lock.
    from fm_disk_replica import DiskReplica
    from fm_disk_store import StorageError
    node = None
    now, armed_cut, armed_fault = 0, None, None

    def exchange(event_type, **fields):
        connection.send({"type": event_type, **fields})
        return connection.recv()

    def at_cut(point, phase=""):
        if armed_cut == (point, phase):
            exchange("cut", point=point, phase=phase,
                     state=_view(node) if node else None)
            raise RuntimeError("PARENT_DID_NOT_KILL_AT_DECLARED_CUT")

    def at_storage(stage, reason):
        if armed_fault and (stage, reason) == (armed_fault["stage"], armed_fault["reason"]):
            if armed_fault["kind"] == "kill":
                exchange("cut", point="storage:" + stage, phase=reason,
                         state=_view(node) if node else None)
                raise RuntimeError("PARENT_DID_NOT_KILL_AT_STORAGE_CUT")
            raise OSError("TEST_INJECTED_STORAGE_FAILURE:" + stage)

    def trace(who, event, info):
        head = node.store.head if node is not None and event == "durable" else None
        exchange("trace", who=who, event=event, info=info, head=head)

    try:
        node = DiskReplica(index, n,
            lambda payload: exchange("sign", payload=payload),
            lambda signed: exchange("verify", signed=signed),
            snapshot, anchor,
            lambda source, kind, data, destination: exchange("emit", source=source,
                kind=kind, data=data, destination=destination),
            lambda: now, trace, directory=directory, create=create,
            trusted_head=trusted_head, fault=at_storage, cut_hook=at_cut)
        connection.send({"type": "ready", "state": _view(node), "head": node.store.head})
        while True:
            request = connection.recv()
            now = request["now"]
            operation, args = request["operation"], request.get("args", [])
            if operation == "close":
                node.close()
                connection.send({"type": "closed"})
                return
            if operation == "set_cut":
                armed_cut = tuple(args)
            elif operation == "set_fault":
                armed_fault = args[0]
            elif operation == "anchors":
                node.anchors.update(deepcopy(args[0]))
            elif operation in ("offer", "receive", "pump", "tick", "retry", "change_view", "restart"):
                getattr(node, operation)(*args)
            elif operation != "inspect":
                raise ValueError("UNKNOWN_TEST_OPERATION")
            connection.send({"type": "result", "state": _view(node), "head": node.store.head})
    except StorageError as error:
        connection.send({"type": "storage_failure", "reason": error.code,
                         "state": _view(node) if node is not None else None})
        if node is not None:
            node.close()
        raise SystemExit(75)
    except EOFError:
        if node is not None:
            node.close()
        raise SystemExit(76)
    finally:
        connection.close()


class ChildError(RuntimeError):
    pass


class ProcessReplica:
    def __init__(self, sim, index, directory):
        self.sim, self.index, self.n = sim, index, sim.n
        self.f = (self.n - 1) // 3
        self.q = 2 * self.f + 1
        self.directory = str(directory)
        self.proofs = Proofs(sim.n, sim.authentication.verify)
        self.witness = None
        self.process = self.connection = None
        self.state = {}
        self.last_cut = self.failure = None
        self._spawn(create=True)
        self.call("anchors", sim.initial_anchors)

    def __getattr__(self, name):
        if name in self.state:
            return self.state[name]
        raise AttributeError(name)

    @property
    def record(self):
        return self.d["records"].get(self.d["sequence"])

    @property
    def instance(self):
        return deepcopy(self.record["instance"])

    @property
    def application(self):
        return Application(self.d["snapshot"])

    def _spawn(self, create):
        parent, child = multiprocessing.get_context("spawn").Pipe()
        self.connection = parent
        self.process = multiprocessing.get_context("spawn").Process(target=_worker,
            args=(child, self.index, self.n, self.sim.initial_snapshot,
                  self.sim.initial_anchor, self.directory, create, self.witness))
        self.process.start()
        child.close()
        self._await()

    def _join(self, expected, reason):
        self.process.join(CALL_SECONDS)
        if self.process.is_alive():
            self.process.kill()
            self.process.join(CALL_SECONDS)
            self.sim.exits.append({"node": self.index, "exit": self.process.exitcode,
                                   "reason": "UNEXPECTED_JOIN_TIMEOUT"})
            raise ChildError("CHILD_DID_NOT_EXIT")
        observation = {"node": self.index, "exit": self.process.exitcode,
                       "reason": reason}
        self.sim.exits.append(observation)
        self.sim.record(self.index, "child_exit", observation)
        self.connection.close()
        if self.process.exitcode != expected:
            raise ChildError("UNEXPECTED_CHILD_EXIT:" + repr(observation))

    def _await(self):
        deadline = time.monotonic() + CALL_SECONDS
        while True:
            if not self.connection.poll(max(0, deadline - time.monotonic())):
                if self.process.is_alive():
                    self.process.kill()
                self._join(-signal.SIGKILL, "UNEXPECTED_REQUEST_TIMEOUT")
                raise ChildError("CHILD_REQUEST_TIMEOUT")
            try:
                message = self.connection.recv()
            except EOFError as error:
                self.process.join(CALL_SECONDS)
                self.sim.exits.append({"node": self.index, "exit": self.process.exitcode,
                                       "reason": "UNEXPECTED_CHILD_EOF"})
                self.state["alive"] = False
                self.connection.close()
                raise ChildError("UNEXPECTED_CHILD_EOF:" + str(self.process.exitcode)) from error
            kind = message["type"]
            if kind in ("ready", "result"):
                self.state = message["state"]
                self.witness = message["head"]
                return message
            if kind == "sign":
                result = self.sim.authentication.signer(self.index)(message["payload"])
            elif kind == "verify":
                result = self.sim.authentication.verify(message["signed"])
            elif kind == "trace":
                self.sim.record(message["who"], message["event"], message["info"])
                if message["head"] is not None:
                    self.witness = message["head"]
                result = None
            elif kind == "emit":
                self.sim.send(message["source"], message["kind"], message["data"], message["destination"])
                result = None
            elif kind == "cut":
                if message["state"] is not None:
                    self.state = message["state"]
                self.last_cut = (message["point"], message["phase"])
                self.process.kill()
                self._join(-signal.SIGKILL, ":".join(self.last_cut))
                self.state["alive"] = False
                return message
            elif kind == "storage_failure":
                if message["state"] is not None:
                    self.state = message["state"]
                self.failure = message["reason"]
                self._join(75, self.failure)
                self.state["alive"] = False
                return message
            elif kind == "closed":
                self._join(0, "clean_shutdown")
                self.state["alive"] = False
                return message
            else:
                raise ChildError("UNKNOWN_CHILD_MESSAGE")
            self.connection.send(result)

    def call(self, operation, *args):
        if not self.process.is_alive():
            return None
        self.connection.send({"operation": operation, "args": args, "now": self.sim.now})
        return self._await()

    def offer(self, body): return self.call("offer", body)
    def receive(self, kind, data, source): return self.call("receive", kind, data, source)
    def pump(self): return self.call("pump")
    def tick(self): return self.call("tick")
    def retry(self): return self.call("retry")
    def change_view(self, view): return self.call("change_view", view)

    def restart(self):
        if self.process.is_alive():
            raise ChildError("REFUSE_SECOND_OWNER")
        self._spawn(create=False)
        if self.process.is_alive():
            self.call("restart")

    def close(self):
        if self.process.is_alive():
            self.call("close")


class ProcessSimulator(Simulator):
    def __init__(self, snapshot, directory, **kwargs):
        super().__init__(snapshot, **kwargs)
        self.exits = []
        self.nodes = []
        try:
            for index in range(self.n):
                self.nodes.append(ProcessReplica(self, index, Path(directory) / str(index)))
        except BaseException:
            self.close()
            raise

    def close(self):
        for node in self.nodes:
            node.close()

    def record_result(self, case, checker_result):
        result = {"case": case, "exits": deepcopy(self.exits), "checker": checker_result}
        PROCESS_RESULTS.append(result)
        print("STORAGE_PROCESS_RESULT " + json.dumps(result, sort_keys=True), flush=True)


class DiskSimulator(Simulator):
    """Same-process scheduling with real disk adapters for bounded-load checks.

    This is explicitly NOT the process-kill fixture; see ProcessSimulator.
    """
    def __init__(self, snapshot, directory, **kwargs):
        from fm_disk_replica import DiskReplica
        super().__init__(snapshot, **kwargs)
        self.nodes = []
        for index in range(self.n):
            node = DiskReplica(index, self.n, self.authentication.signer(index),
                self.authentication.verify, snapshot, self.initial_anchor,
                self.send, lambda: self.now, self.record,
                directory=Path(directory) / str(index), create=True)
            node.anchors.update(deepcopy(self.initial_anchors))
            self.nodes.append(node)

    def close(self):
        for node in self.nodes:
            node.close()
