"""Protocol/privacy bounds tested without opening sockets or loading a model."""

import contextlib
import io
import json
import threading
import unittest
from email.message import Message
from types import SimpleNamespace

from contrib.b3ai.scratch.server import (
    ApiError, BoundedScratchServer, DEFAULT_GENERATION_TOKENS, EXPERIMENTAL_HEADER,
    MAX_BODY_BYTES, MAX_GENERATION_TOKENS, MAX_MESSAGES, MODEL_NAME,
    ScratchRequestHandler, ScratchService,
    chat_prompt, decode_request, literal_loopback, main,
)


class BytesForTests:
    def encode(self, text):
        return list(text.encode("utf-8"))

    def decode(self, ids):
        return bytes(ids).decode("utf-8", errors="replace")


class ModelDouble:
    parameter_count = 99

    def __init__(self, output=b"Model bytes.\n", context_length=1024):
        self.config = SimpleNamespace(context_length=context_length)
        self.output = output
        self.calls = []

    def generate(self, prompt_ids, max_new_tokens, temperature=0, seed=42):
        self.calls.append((list(prompt_ids), max_new_tokens, temperature, seed))
        return list(prompt_ids) + list(self.output[:max_new_tokens])


def service(model=None):
    return ScratchService(model or ModelDouble(), BytesForTests(),
                          {"stage": "sanity-pretraining", "readiness": "experimental"})


def request(**extras):
    body = {"model": MODEL_NAME, "messages": [{"role": "user", "content": "What is B3?"}]}
    body.update(extras)
    return body


class DirectHandler(ScratchRequestHandler):
    """Exercise the real route/body logic with in-memory HTTP inputs."""

    def __init__(self, backend, body=b"", path="/v1/chat/completions", headers=None):
        self.server = SimpleNamespace(service=backend)
        self.path = path
        self.headers = Message()
        default = [("Content-Length", str(len(body))), ("Content-Type", "application/json")]
        for name, value in (default if headers is None else headers):
            self.headers[name] = value
        self.rfile = io.BytesIO(body)
        self.response = None

    def _send_json(self, status, payload):
        self.response = status, payload


class ScratchServerTests(unittest.TestCase):
    def assert_api_error(self, backend, payload, status=400, code=None):
        with self.assertRaises(ApiError) as caught:
            backend.completion(payload)
        self.assertEqual(caught.exception.status, status)
        if code:
            self.assertEqual(caught.exception.code, code)
        return caught.exception

    def test_returns_only_generated_bytes_and_marks_experimental(self):
        model = ModelDouble(b"Untrained sample, not a verified answer.\n")
        backend = service(model)
        result = backend.completion(request(max_tokens=64, temperature=0.3, seed=71))
        self.assertEqual(result["choices"][0]["message"], {
            "role": "assistant", "content": "Untrained sample, not a verified answer.\n"})
        self.assertEqual(result["choices"][0]["finish_reason"], "stop")
        self.assertFalse(result["experimental"]["tool_calling"])
        self.assertEqual(result["experimental"]["readiness"], "experimental")
        prompt_ids, count, temperature, seed = model.calls[0]
        self.assertEqual(bytes(prompt_ids).decode(), "\nUser: What is B3?\nAssistant: ")
        self.assertEqual((count, temperature, seed), (64, 0.3, 71))
        self.assertEqual(result["usage"]["prompt_tokens"], len(prompt_ids))
        self.assertEqual(result["usage"]["total_tokens"], len(prompt_ids) + len(model.output))

    def test_prompt_matches_teacher_format_and_keeps_system_evidence(self):
        messages = [{"role": "system", "content": "Source: baseline-revision"},
                    {"role": "user", "content": "First question"},
                    {"role": "assistant", "content": "Earlier answer"},
                    {"role": "user", "content": "नमस्ते B3"}]
        expected = "\nSystem: Source: baseline-revision\n\nUser: First question\nAssistant: Earlier answer\n\nUser: नमस्ते B3\nAssistant: "
        self.assertEqual(chat_prompt(messages), expected)
        model = ModelDouble()
        service(model).completion(request(messages=messages))
        self.assertEqual(bytes(model.calls[0][0]).decode(), expected)

    def test_tools_never_reach_model_even_when_empty(self):
        model = ModelDouble()
        backend = service(model)
        for field in ("tools", "tool_choice", "functions", "function_call"):
            with self.subTest(field=field):
                self.assert_api_error(backend, request(**{field: []}), code="tools_not_qualified")
        self.assertEqual(model.calls, [])

    def test_tool_messages_rich_content_and_unknown_fields_are_rejected(self):
        model = ModelDouble()
        backend = service(model)
        bad_messages = [
            [{"role": "tool", "content": "SECRET", "tool_call_id": "1"}],
            [{"role": "user", "content": [{"type": "text", "text": "SECRET"}]}],
            [{"role": "user", "content": "hello", "name": "SECRET"}],
            [{"role": "assistant", "content": "answer"}],
            [{"role": "system", "content": "alone"}],
            [{"role": "user", "content": "q"}, {"role": "system", "content": "late"}],
            [{"role": "user", "content": "q"}, {"role": "user", "content": "q2"}],
        ]
        for messages in bad_messages:
            with self.subTest(messages=messages):
                error = self.assert_api_error(backend, request(messages=messages))
                self.assertNotIn("SECRET", json.dumps(error.payload()))
        self.assert_api_error(backend, request(response_format={}))
        self.assertEqual(model.calls, [])

    def test_context_overflow_is_rejected_without_truncation(self):
        model = ModelDouble(context_length=100)
        backend = service(model)
        error = self.assert_api_error(backend, request(messages=[{"role": "system", "content": "Evidence " * 15},
            {"role": "user", "content": "q"}]), code="context_length_exceeded")
        self.assertIn("No messages were truncated", error.message)
        self.assertEqual(model.calls, [])
        self.assert_api_error(backend, request(messages=[{"role": "user", "content": "中" * 30}]),
                              code="context_length_exceeded")

    def test_request_and_generation_limits(self):
        backend = service()
        for extras in ({"max_tokens": 0}, {"max_tokens": True},
                       {"max_tokens": MAX_GENERATION_TOKENS + 1},
                       {"temperature": float("nan")}, {"temperature": True},
                       {"temperature": 3}, {"temperature": 10**400}, {"seed": -1}, {"seed": True},
                       {"seed": 2**32}, {"stream": True}, {"stream": "false"}):
            with self.subTest(extras=extras):
                self.assert_api_error(backend, request(**extras))
        self.assert_api_error(backend, request(model="not-an-experiment"), status=404)
        self.assert_api_error(backend, request(messages=[{"role": "user", "content": "x" * 9000}]), status=413)
        self.assert_api_error(backend, request(messages=[{"role": "system", "content": "s"}] * (MAX_MESSAGES + 1)))
        self.assert_api_error(backend, request(messages=[{"role": "user", "content": "\ud800"}]))

    def test_empty_or_failed_generation_never_becomes_canned_success(self):
        class FailedModel(ModelDouble):
            def generate(self, *args, **kwargs):
                raise RuntimeError("PRIVATE_CHECKPOINT_PATH AND SECRET")
        for model in (ModelDouble(b""), ModelDouble(b" \n\t"), FailedModel()):
            error = self.assert_api_error(service(model), request(), status=502, code="generation_failed")
            self.assertNotIn("SECRET", json.dumps(error.payload()))
            self.assertNotIn("choices", error.payload())

    def test_wrong_prefix_and_excess_output_are_rejected(self):
        class WrongPrefix(ModelDouble):
            def generate(self, prompt_ids, **kwargs):
                return [1] + prompt_ids[1:] + [65]
        class TooLong(ModelDouble):
            def generate(self, prompt_ids, **kwargs):
                return prompt_ids + [65] * (DEFAULT_GENERATION_TOKENS + 1)
        for model in (WrongPrefix(), TooLong()):
            self.assert_api_error(service(model), request(), status=502, code="generation_failed")

    def test_generation_is_serialized_and_busy_request_does_not_queue(self):
        started, release = threading.Event(), threading.Event()
        class SlowModel(ModelDouble):
            def generate(self, *args, **kwargs):
                started.set()
                if not release.wait(2):
                    raise RuntimeError("test timeout")
                return super().generate(*args, **kwargs)
        backend = service(SlowModel())
        outcomes = []
        thread = threading.Thread(target=lambda: outcomes.append(backend.completion(request())))
        thread.start()
        try:
            self.assertTrue(started.wait(1))
            self.assert_api_error(backend, request(), status=429, code="model_busy")
        finally:
            release.set()
            thread.join(2)
        self.assertFalse(thread.is_alive())
        self.assertEqual(len(outcomes), 1)
        self.assertEqual(len(backend.model.calls), 1)
        backend.completion(request())  # The lock is released after completion.

    def test_models_report_unqualified_capabilities(self):
        backend = service()
        handler = DirectHandler(backend, path="/v1/models")
        handler.do_GET()
        status, payload = handler.response
        self.assertEqual(status, 200)
        model = payload["data"][0]
        self.assertEqual(model["id"], MODEL_NAME)
        self.assertEqual(model["stage"], "sanity-pretraining")
        self.assertFalse(model["capabilities"]["tool_calling"])
        self.assertEqual(model["parameter_count"], 99)

    def test_handler_checks_body_before_forwarding(self):
        backend = service()
        body = json.dumps(request()).encode()
        handler = DirectHandler(backend, body)
        handler.do_POST()
        self.assertEqual(handler.response[0], 200)
        cases = [([], 411),
                 ([("Content-Length", "-1")], 400),
                 ([("Content-Length", "1"), ("Content-Length", "2")], 400),
                 ([("Content-Length", str(MAX_BODY_BYTES + 1))], 413),
                 ([("Content-Length", str(len(body))), ("Content-Type", "text/plain")], 415),
                 ([("Content-Length", str(len(body))), ("Transfer-Encoding", "chunked")], 400)]
        for headers, expected in cases:
            with self.subTest(headers=headers):
                handler = DirectHandler(backend, body, headers=headers)
                handler.do_POST()
                self.assertEqual(handler.response[0], expected)
        self.assertEqual(len(backend.model.calls), 1)

    def test_malformed_json_truncated_body_and_unknown_routes_are_sanitized(self):
        backend = service()
        for body in (b'{"SECRET":', b'\xff', b'{"model":1,"model":2}', b'{"x":NaN}'):
            with self.subTest(body=body):
                handler = DirectHandler(backend, body)
                handler.do_POST()
                self.assertEqual(handler.response[0], 400)
                self.assertNotIn("SECRET", json.dumps(handler.response[1]))
        handler = DirectHandler(backend, b"{}", headers=[("Content-Length", "30"), ("Content-Type", "application/json")])
        handler.do_POST()
        self.assertEqual(handler.response[0], 400)
        handler = DirectHandler(backend, b"{}", path="/v1/SECRET")
        handler.do_POST()
        self.assertEqual(handler.response[0], 404)
        self.assertNotIn("SECRET", json.dumps(handler.response[1]))
        self.assertEqual(backend.model.calls, [])

    def test_literal_loopback_only_and_explicit_cli_activation(self):
        self.assertTrue(literal_loopback("127.0.0.1").is_loopback)
        self.assertTrue(literal_loopback("::1").is_loopback)
        for host in ("localhost", "0.0.0.0", "::", "192.168.1.2", "https://127.0.0.1"):
            with self.subTest(host=host), self.assertRaises(ValueError):
                literal_loopback(host)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as caught:
            main(["--checkpoint", "never-loaded.npz"])
        self.assertEqual(caught.exception.code, 2)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            main(["--checkpoint", "never-loaded.npz", "--allow-experimental", "--host", "0.0.0.0"])

    def test_checkpoint_metadata_must_declare_experimental_stage(self):
        for metadata in ({}, {"stage": "chat-trained", "readiness": "experimental"},
                         {"stage": "sanity-pretraining", "readiness": "production"}):
            with self.subTest(metadata=metadata), self.assertRaises(ValueError):
                ScratchService(ModelDouble(), BytesForTests(), metadata)
        with self.assertRaises(ValueError):
            ScratchService(ModelDouble(), BytesForTests(),
                           {"stage": "sanity-pretraining", "readiness": "experimental"}, "b3-assistant")

    def test_response_headers_mark_experimental_and_do_not_cache(self):
        headers = {}
        writer = SimpleNamespace(
            send_response=lambda status: headers.update(status=status),
            send_header=lambda name, value: headers.update({name: value}),
            end_headers=lambda: None, wfile=io.BytesIO(), close_connection=False,
        )
        ScratchRequestHandler._send_json(writer, 200, {"content": "नमस्ते"})
        self.assertEqual(headers["X-B3-Experimental"], EXPERIMENTAL_HEADER)
        self.assertEqual(headers["Cache-Control"], "no-store")
        self.assertEqual(headers["Content-Length"], str(len(writer.wfile.getvalue())))
        self.assertTrue(writer.close_connection)
        self.assertEqual(json.loads(writer.wfile.getvalue()), {"content": "नमस्ते"})
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            ScratchRequestHandler.log_message(writer, "Authorization: %s", "SECRET")
        self.assertEqual(output.getvalue(), "")

    def test_transport_thread_limit_rejects_without_starting_more_threads(self):
        server = object.__new__(BoundedScratchServer)
        server._request_slots = threading.BoundedSemaphore(1)
        self.assertTrue(server._request_slots.acquire(blocking=False))
        closed = []
        server.shutdown_request = lambda request: closed.append(request)
        output = []
        socket_double = SimpleNamespace(settimeout=lambda _: None, sendall=output.append)
        server.process_request(socket_double, ("127.0.0.1", 0))
        self.assertEqual(closed, [socket_double])
        self.assertEqual(len(output), 1)
        response = output[0]
        self.assertTrue(response.startswith(b"HTTP/1.1 503"))
        self.assertIn(EXPERIMENTAL_HEADER.encode(), response)
        payload = json.loads(response.split(b"\r\n\r\n", 1)[1])
        self.assertIn("error", payload)


if __name__ == "__main__":
    unittest.main()
