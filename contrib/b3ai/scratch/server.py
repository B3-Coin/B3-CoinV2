"""Opt-in loopback serving for an unqualified scratch model, never a tool agent."""

from __future__ import annotations

import argparse
import ipaddress
import json
import math
import numbers
import re
import socket
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any

MODEL_NAME = "b3-scratch-experiment"
EXPERIMENTAL_HEADER = "sanity-pretraining; not-qualified-for-tasks-or-tools"
MAX_BODY_BYTES = 32 * 1024
MAX_MESSAGE_BYTES = 8 * 1024
MAX_PROMPT_BYTES = 16 * 1024
MAX_OUTPUT_BYTES = 8 * 1024
MAX_MESSAGES = 16
MAX_CONTEXT_TOKENS = 4096
MAX_GENERATION_TOKENS = 128
DEFAULT_GENERATION_TOKENS = 32
MAX_REQUEST_THREADS = 4
REQUEST_TIMEOUT_SECONDS = 10


class ApiError(Exception):
    """A deliberately public error containing no submitted text or secrets."""

    def __init__(self, status: int, message: str, code: str = "invalid_request"):
        super().__init__(message)
        self.status = status
        self.message = message
        self.code = code

    def payload(self) -> dict[str, Any]:
        return {"error": {"message": self.message, "type": self.code, "code": self.code}}


def literal_loopback(host: str) -> ipaddress.IPv4Address | ipaddress.IPv6Address:
    try:
        address = ipaddress.ip_address(host)
    except ValueError as exc:
        raise ValueError("Bind address must be a literal loopback IP address.") from exc
    if not address.is_loopback:
        raise ValueError("Bind address must be a literal loopback IP address.")
    return address


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ApiError(400, "Duplicate JSON fields are unsupported.")
        result[key] = value
    return result


def _reject_nonfinite_json(_value: str) -> None:
    raise ValueError("Non-finite JSON numbers are unsupported.")


def decode_request(body: bytes) -> Any:
    if not body or len(body) > MAX_BODY_BYTES:
        raise ApiError(413, "Request body is empty or exceeds the experimental limit.")
    try:
        return json.loads(body.decode("utf-8"), object_pairs_hook=_unique_object,
                          parse_constant=_reject_nonfinite_json)
    except ApiError:
        raise
    except (UnicodeDecodeError, ValueError, RecursionError) as exc:
        raise ApiError(400, "Request must contain valid UTF-8 JSON.") from exc


def _byte_length(text: str) -> int:
    try:
        return len(text.encode("utf-8"))
    except UnicodeEncodeError as exc:
        raise ApiError(400, "Message text must be valid Unicode.") from exc


def chat_prompt(messages: Any) -> str:
    if not isinstance(messages, list) or not 1 <= len(messages) <= MAX_MESSAGES:
        raise ApiError(400, "Provide a bounded messages array ending with a user message.")
    parts = []
    expected = "user"
    seen_user = False
    for message in messages:
        if not isinstance(message, dict) or set(message) != {"role", "content"}:
            raise ApiError(400, "Only plain-text role/content messages are supported; tools are not qualified.")
        role = message["role"]
        content = message["content"]
        if not isinstance(role, str) or role not in {"system", "user", "assistant"}:
            raise ApiError(400, "Only system, user and assistant text roles are supported; tools are not qualified.")
        if not isinstance(content, str) or not content.strip():
            raise ApiError(400, "Each message must contain nonempty plain text.")
        if _byte_length(content) > MAX_MESSAGE_BYTES:
            raise ApiError(413, "A message exceeds the experimental text limit.")
        if role == "system":
            if seen_user:
                raise ApiError(400, "System messages must precede the conversation.")
            parts.append(f"\nSystem: {content}\n")
        elif role == "user":
            if expected != "user":
                raise ApiError(400, "User and assistant messages must alternate.")
            seen_user = True
            expected = "assistant"
            parts.append(f"\nUser: {content}\nAssistant: ")
        else:
            if not seen_user or expected != "assistant":
                raise ApiError(400, "An assistant message must follow a user message.")
            expected = "user"
            parts.append(f"{content}\n")
    if not seen_user or expected != "assistant":
        raise ApiError(400, "The conversation must end with a user message.")
    prompt = "".join(parts)
    if _byte_length(prompt) > MAX_PROMPT_BYTES:
        raise ApiError(413, "The complete prompt exceeds the experimental text limit.")
    return prompt


class ScratchService:
    """One serialized model with strict request bounds and no tool dispatcher."""

    def __init__(self, model: Any, tokenizer: Any, metadata: dict[str, Any], model_name: str = MODEL_NAME):
        if model_name != MODEL_NAME:
            raise ValueError(f"Experimental model name must be {MODEL_NAME}.")
        if not isinstance(metadata, dict) or metadata.get("stage") != "sanity-pretraining" or metadata.get("readiness") != "experimental":
            raise ValueError("Checkpoint must declare sanity-pretraining stage and experimental readiness.")
        context_length = model.config.context_length
        if isinstance(context_length, bool) or not isinstance(context_length, numbers.Integral) or context_length < 1:
            raise ValueError("Checkpoint has an invalid context length.")
        self.model = model
        self.tokenizer = tokenizer
        self.model_name = model_name
        self.context_length = min(int(context_length), MAX_CONTEXT_TOKENS)
        self._forward_lock = threading.Lock()

    def models(self) -> dict[str, Any]:
        return {"object": "list", "data": [{
            "id": self.model_name, "object": "model", "owned_by": "b3-scratch-local",
            "stage": "sanity-pretraining", "readiness": "experimental",
            "parameter_count": int(self.model.parameter_count),
            "capabilities": {"chat_completions": "experimental", "tool_calling": False},
        }]}

    def completion(self, request: Any) -> dict[str, Any]:
        if not isinstance(request, dict):
            raise ApiError(400, "The request must be a JSON object.")
        if any(key in request for key in ("tools", "tool_choice", "functions", "function_call")):
            raise ApiError(400, "This scratch checkpoint is not qualified for tool calling; disable tools.", "tools_not_qualified")
        if set(request) - {"model", "messages", "stream", "max_tokens", "temperature", "seed"}:
            raise ApiError(400, "Unsupported request fields for this experimental adapter.")
        if request.get("model") != self.model_name:
            raise ApiError(404, "The requested experimental model is unavailable.", "model_not_found")
        if request.get("stream", False) is not False:
            raise ApiError(400, "Streaming is unsupported by this experimental adapter.")
        max_tokens = request.get("max_tokens", DEFAULT_GENERATION_TOKENS)
        if isinstance(max_tokens, bool) or not isinstance(max_tokens, int) or not 1 <= max_tokens <= MAX_GENERATION_TOKENS:
            raise ApiError(400, "max_tokens must be a positive integer within the experimental limit.")
        temperature = request.get("temperature", 0)
        if isinstance(temperature, bool) or not isinstance(temperature, (int, float)) or not 0 <= temperature <= 2 or not math.isfinite(temperature):
            raise ApiError(400, "temperature must be a finite number between zero and two.")
        seed = request.get("seed", 42)
        if isinstance(seed, bool) or not isinstance(seed, int) or not 0 <= seed <= 2**32 - 1:
            raise ApiError(400, "seed must be an unsigned 32-bit integer.")
        prompt = chat_prompt(request.get("messages"))
        prompt_ids = list(self.tokenizer.encode(prompt))
        if not prompt_ids or len(prompt_ids) + max_tokens > self.context_length:
            raise ApiError(400, "The full prompt plus requested output exceeds this checkpoint's context. No messages were truncated.", "context_length_exceeded")
        if not self._forward_lock.acquire(blocking=False):
            raise ApiError(429, "The experimental model is busy; retry after the current request.", "model_busy")
        try:
            generated = list(self.model.generate(prompt_ids, max_new_tokens=max_tokens,
                                                 temperature=temperature, seed=seed))
            if any(isinstance(token, bool) or not isinstance(token, numbers.Integral) or not 0 <= token < 256 for token in generated):
                raise ValueError("Invalid generated byte token.")
            generated = [int(token) for token in generated]
            if generated[:len(prompt_ids)] != prompt_ids:
                raise ValueError("Generated prefix mismatch.")
            output_ids = generated[len(prompt_ids):]
            if not output_ids or len(output_ids) > max_tokens:
                raise ValueError("Invalid generation length.")
            content = self.tokenizer.decode(output_ids)
            if not isinstance(content, str) or not content.strip() or _byte_length(content) > MAX_OUTPUT_BYTES:
                raise ValueError("Empty or oversized generation.")
        except Exception as exc:
            raise ApiError(502, "The experimental model could not produce a usable response.", "generation_failed") from exc
        finally:
            self._forward_lock.release()
        return {
            "id": "chatcmpl-" + uuid.uuid4().hex, "object": "chat.completion",
            "created": int(time.time()), "model": self.model_name,
            "choices": [{"index": 0, "message": {"role": "assistant", "content": content},
                         "finish_reason": "length" if len(output_ids) == max_tokens else "stop"}],
            "usage": {"prompt_tokens": len(prompt_ids), "completion_tokens": len(output_ids),
                      "total_tokens": len(prompt_ids) + len(output_ids)},
            "experimental": {"stage": "sanity-pretraining", "readiness": "experimental", "tool_calling": False},
        }


class ScratchRequestHandler(BaseHTTPRequestHandler):
    server_version = "B3ScratchExperimental/1"
    sys_version = ""
    protocol_version = "HTTP/1.1"

    def setup(self) -> None:
        self.request.settimeout(REQUEST_TIMEOUT_SECONDS)
        super().setup()

    def log_message(self, _format: str, *args: Any) -> None:
        # Do not log message bodies, request URLs, or Authorization headers.
        pass

    def _send_json(self, status: int, payload: dict[str, Any]) -> None:
        body = json.dumps(payload, ensure_ascii=True, allow_nan=False, separators=(",", ":")).encode("ascii")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-B3-Experimental", EXPERIMENTAL_HEADER)
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        self.wfile.write(body)

    def send_error(self, code: int, message: str | None = None, explain: str | None = None) -> None:
        self._send_json(code, ApiError(code, "Invalid or unsupported HTTP request.").payload())

    def do_GET(self) -> None:
        if self.path != "/v1/models":
            self._send_json(404, ApiError(404, "This experimental route is unavailable.").payload())
            return
        self._send_json(200, self.server.service.models())

    def do_POST(self) -> None:
        try:
            if self.path != "/v1/chat/completions":
                raise ApiError(404, "This experimental route is unavailable.")
            if self.headers.get("Transfer-Encoding") is not None:
                raise ApiError(400, "Transfer encodings are unsupported.")
            lengths = self.headers.get_all("Content-Length", [])
            if not lengths:
                raise ApiError(411, "A bounded Content-Length is required.")
            if len(lengths) != 1 or not re.fullmatch(r"[0-9]{1,8}", lengths[0]):
                raise ApiError(400, "Content-Length is invalid.")
            length = int(lengths[0])
            if not 1 <= length <= MAX_BODY_BYTES:
                raise ApiError(413, "Request body exceeds the experimental limit.")
            if self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower() != "application/json":
                raise ApiError(415, "Content-Type must be application/json.")
            body = self.rfile.read(length)
            if len(body) != length:
                raise ApiError(400, "The request body is incomplete.")
            result = self.server.service.completion(decode_request(body))
            self._send_json(200, result)
        except ApiError as exc:
            self._send_json(exc.status, exc.payload())
        except (OSError, ValueError, RecursionError):
            self._send_json(400, ApiError(400, "The request could not be read.").payload())
        except Exception:
            self._send_json(500, ApiError(500, "The experimental request failed.").payload())


class BoundedScratchServer(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = MAX_REQUEST_THREADS

    def __init__(self, address: tuple[str, int], service: ScratchService):
        ip = literal_loopback(address[0])
        self.address_family = socket.AF_INET6 if ip.version == 6 else socket.AF_INET
        self.service = service
        self._request_slots = threading.BoundedSemaphore(MAX_REQUEST_THREADS)
        super().__init__(address, ScratchRequestHandler)

    def process_request(self, request: Any, client_address: Any) -> None:
        if not self._request_slots.acquire(blocking=False):
            payload = json.dumps(ApiError(503, "The experimental request limit is reached.").payload(), separators=(",", ":")).encode("ascii")
            try:
                request.settimeout(1)
                request.sendall(b"HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nX-B3-Experimental: " + EXPERIMENTAL_HEADER.encode("ascii") + b"\r\nConnection: close\r\nContent-Length: " + str(len(payload)).encode("ascii") + b"\r\n\r\n" + payload)
            except OSError:
                pass
            finally:
                self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self._request_slots.release()
            raise

    def process_request_thread(self, request: Any, client_address: Any) -> None:
        try:
            super().process_request_thread(request, client_address)
        finally:
            self._request_slots.release()

    def handle_error(self, request: Any, client_address: Any) -> None:
        # A failed local request must not print request content or credentials.
        pass


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Experimental loopback adapter for B3's sanity-pretraining checkpoint. Not qualified for tasks or tools.")
    parser.add_argument("--checkpoint", required=True)
    parser.add_argument("--model", choices=[MODEL_NAME], default=MODEL_NAME)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--allow-experimental", action="store_true")
    args = parser.parse_args(argv)
    if not args.allow_experimental:
        parser.error("Serving requires explicit --allow-experimental; this checkpoint is not qualified for tasks or tools.")
    try:
        literal_loopback(args.host)
    except ValueError as exc:
        parser.error(str(exc))
    if not 1 <= args.port <= 65535:
        parser.error("Port must be between 1 and 65535.")
    try:
        from .checkpoint import load_checkpoint
        from .tokenizer import ByteTokenizer
        model, _optimizer, metadata = load_checkpoint(args.checkpoint)
        service = ScratchService(model, ByteTokenizer(), metadata, args.model)
    except Exception:
        parser.error("Could not load an experimental sanity-pretraining checkpoint with valid metadata.")
    with BoundedScratchServer((args.host, args.port), service) as server:
        print(f"Experimental {MODEL_NAME} on loopback {args.host}:{args.port}; not qualified for tasks or tools.", flush=True)
        try:
            server.serve_forever(poll_interval=0.25)
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
