"""Extract the leading static manifest table from a Lua plugin entry."""

import bisect
import re


MAX_SOURCE_BYTES = 128 * 1024
MAX_DEPTH = 32
MAX_FIELDS = 4096
IDENTIFIER = re.compile(r"[a-zA-Z_][a-zA-Z_0-9]*")
INTEGER = re.compile(r"-?(?:0|[1-9][0-9]*)")
LONG_OPEN = re.compile(r"\[(=*)\[")
KEYWORDS = frozenset(
    "and break do else elseif end false for function global goto if in local nil "
    "not or repeat return then true until while".split()
)
ESCAPES = {
    "a": "\a", "b": "\b", "f": "\f", "n": "\n", "r": "\r",
    "t": "\t", "v": "\v", "\\": "\\", '"': '"', "'": "'",
}


class ManifestError(ValueError):
    """Invalid static declaration, with the offending source location."""


class _Parser:
    def __init__(self, source):
        self.source = source
        self.pos = 0
        self.lines = [0] + [match.end() for match in re.finditer(r"\r\n|\n\r|[\r\n]", source)]
        self.fields_left = MAX_FIELDS

    def fail(self, message, pos=None):
        pos = self.pos if pos is None else pos
        line = bisect.bisect_right(self.lines, pos)
        raise ManifestError(f"{line}:{pos - self.lines[line - 1] + 1}: {message}")

    def skip(self):
        while self.pos < len(self.source):
            if self.source[self.pos] in " \t\r\n\v\f":
                self.pos += 1
                continue
            if not self.source.startswith("--", self.pos):
                return
            self.pos += 2
            opening = LONG_OPEN.match(self.source, self.pos)
            if opening:
                closing = "]" + opening[1] + "]"
                end = self.source.find(closing, opening.end())
                if end == -1:
                    self.fail("unterminated long comment")
                self.pos = end + len(closing)
            else:
                while self.pos < len(self.source) and self.source[self.pos] not in "\r\n":
                    self.pos += 1

    def take(self, literal):
        self.skip()
        if not self.source.startswith(literal, self.pos):
            return False
        self.pos += len(literal)
        return True

    def expect(self, literal):
        if not self.take(literal):
            self.fail(f"expected {literal!r}")

    def name(self):
        self.skip()
        return IDENTIFIER.match(self.source, self.pos)

    def string(self):
        start = self.pos
        quote = self.source[self.pos]
        self.pos += 1
        result = []
        while self.pos < len(self.source):
            char = self.source[self.pos]
            self.pos += 1
            if char == quote:
                return "".join(result)
            if char in "\r\n":
                break
            if char == "\\":
                escape = self.source[self.pos:self.pos + 1]
                if escape not in ESCAPES:
                    self.fail("unsupported escape in manifest string")
                char = ESCAPES[escape]
                self.pos += 1
            result.append(char)
        self.fail("unterminated manifest string", start)

    def value(self, depth):
        self.skip()
        char = self.source[self.pos:self.pos + 1]
        if char == "{":
            return self.table(depth + 1)
        if char in ('"', "'"):
            return self.string()
        name = self.name()
        if name and name[0] in ("true", "false"):
            self.pos = name.end()
            return name[0] == "true"
        integer = INTEGER.match(self.source, self.pos)
        if integer:
            digits = integer[0]
            # Keep the same exact integer range across JSON implementations.
            if len(digits.lstrip("-")) > 16 or abs(int(digits)) > 2**53 - 1:
                self.fail("manifest integer outside the exact JSON range")
            self.pos = integer.end()
            return int(digits)
        self.fail("expected a static table, string, integer or boolean")

    def table(self, depth):
        if depth > MAX_DEPTH:
            self.fail("manifest tables nested too deeply")
        self.expect("{")
        fields = {}
        items = []
        while not self.take("}"):
            if self.fields_left == 0:
                self.fail("too many manifest fields")
            self.fields_left -= 1
            start = self.pos
            name = self.name()
            if name and name[0] not in KEYWORDS:
                self.pos = name.end()
                if self.take("="):
                    key = name[0]
                    if items:
                        self.fail("cannot mix array and object entries", start)
                    if key in fields:
                        self.fail(f"duplicate manifest key {key!r}", start)
                    fields[key] = self.value(depth)
                else:
                    self.fail("expected '=' after manifest key")
            else:
                if fields:
                    self.fail("cannot mix array and object entries", start)
                items.append(self.value(depth))
            if self.take(",") or self.take(";"):
                continue
            self.expect("}")
            break
        return items if items else fields

    def parse(self):
        for expected in ("local", "manifest"):
            name = self.name()
            if not name or name[0] != expected:
                self.fail(f"expected {expected!r} in the leading manifest declaration")
            self.pos = name.end()
        self.expect("=")
        table = self.table(1)
        if not isinstance(table, dict):
            self.fail("manifest must be an object")
        # An expression can continue after '}', even across a newline.
        self.skip()
        suffix = self.source[self.pos:self.pos + 1]
        name = self.name()
        if suffix in set(",.([+-*/%^#&|~=<>'\"{") or (
            name and name[0] in ("and", "or")
        ):
            self.fail("manifest initializer must be a single table literal")
        return table


def extract_manifest(source):
    """Return the authored metadata without loading or executing Lua.

    Object keys use Lua identifiers. Empty tables are objects here and can be
    interpreted as empty arrays by the schema for fields declared as arrays.
    """
    if len(source) > MAX_SOURCE_BYTES:
        raise ManifestError("1:1: plugin entry exceeds the source size limit")
    try:
        text = source.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ManifestError(f"invalid UTF-8 at byte {error.start}") from error
    return _Parser(text).parse()


_ID = re.compile(r"[a-z][a-z0-9-]*(?:\.[a-z][a-z0-9-]*)+")
_VERSION = re.compile(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)")
_COMMAND = re.compile(r"[a-z][a-z0-9-]{0,63}")
_CAPABILITY = re.compile(r"[a-z][a-zA-Z0-9]*(?:\.[a-z][a-zA-Z0-9]*)+")
_VIEW = frozenset(("chat", "approval", "diff", "table", "task", "form", "resource"))


def _require(condition, message):
    if not condition:
        raise ManifestError(message)


def _keys(value, required, optional, label):
    _require(isinstance(value, dict) and required <= value.keys() <= required | optional,
             f"invalid fields in {label}")


def _strings(value, limit, label):
    if value == {}:
        return []
    _require(isinstance(value, list) and len(value) <= limit and
             all(isinstance(item, str) and item for item in value) and len(value) == len(set(value)),
             f"invalid string list in {label}")
    return value


def _text(value, limit, label):
    _require(isinstance(value, str) and 0 < len(value) <= limit and
             not any(ord(char) < 32 or ord(char) == 127 for char in value),
             f"invalid {label}")
    return value


def release_manifest(source, files, checked_path):
    """Validate the Lua declaration and return generated metadata and its members."""
    declaration = extract_manifest(source)
    _keys(declaration, {"id", "version", "api", "publisher", "license", "commands"},
          {"client", "executor", "files"}, "manifest")
    plugin_id = _text(declaration["id"], 128, "plugin id")
    version = _text(declaration["version"], 64, "plugin version")
    _require(_ID.fullmatch(plugin_id) and _VERSION.fullmatch(version), "invalid Lua plugin identity")
    _require(declaration["api"] == "1", "unsupported Lua plugin API")
    manifest = {
        "schemaVersion": 2,
        "id": plugin_id,
        "pluginVersion": version,
        "sdkApiRange": "^1.0.0",
        "bridgeProtocolRange": [1],
        "publisher": _text(declaration["publisher"], 128, "publisher"),
        "license": _text(declaration["license"], 128, "license"),
    }
    members = {"main.lua"}

    def add_file(path, suffix=None):
        path = checked_path(path)
        _require(path in files and path not in {"manifest.json", "integrity.json", "plugin.json"},
                 f"invalid or missing Lua package member: {path}")
        if suffix is not None:
            _require(path.endswith(suffix), f"unexpected file type: {path}")
        if path.endswith(".lua"):
            content = files[path]
            _require(0 < len(content) <= MAX_SOURCE_BYTES and b"\0" not in content and
                     not content.startswith(b"\x1b"), f"invalid Lua source: {path}")
            content.decode("utf-8")
        else:
            lowered = path.lower()
            _require(not lowered.endswith((".js", ".mjs", ".cjs", ".jsx", ".ts", ".tsx",
                                           ".mts", ".cts", ".luac")) and
                     lowered.rsplit("/", 1)[-1] not in {"package.json", "package-lock.json"},
                     f"legacy plugin member: {path}")
        members.add(path)
        return path

    add_file("main.lua", ".lua")
    for side in ("client", "executor"):
        if side not in declaration:
            continue
        definition = declaration[side]
        required = {"requires"} if side == "client" else {"entry", "requires"}
        optional = {"rpc", "programs"} if side == "executor" else {"rpc"}
        _keys(definition, required, optional, side)
        entry = "main.lua" if side == "client" else add_file(definition["entry"], ".lua")
        capabilities = _strings(definition["requires"], 32, side)
        _require(all(len(name) <= 128 and _CAPABILITY.fullmatch(name) for name in capabilities),
                 f"invalid capability in {side}")
        manifest[side] = {"entry": entry, "requires": capabilities}
        methods = _strings(definition.get("rpc", []), 16, f"{side} RPC methods")
        _require(not methods or "session.rpc" in capabilities, f"{side} RPC requires session.rpc")
        _require(all(len(name) <= 128 and "." in name and
                     re.fullmatch(r"[a-z][a-zA-Z0-9._-]*", name) for name in methods),
                 f"invalid RPC method in {side}")
        if methods:
            manifest[side]["rpc"] = methods
        if side == "executor":
            programs = _strings(definition.get("programs", []), 16, "executor programs")
            process_capabilities = {"process.spawn", "process.pipes"}
            _require((bool(programs) and process_capabilities <= set(capabilities)) or
                     (not programs and not process_capabilities.intersection(capabilities)),
                     "executor process permissions require explicit programs and pipe access")
            _require(all(_COMMAND.fullmatch(program) for program in programs),
                     "invalid executor program")
            if programs:
                manifest[side]["programs"] = programs
    _require("client" in manifest or "executor" in manifest, "plugin requires a role")

    paths = set()
    commands = declaration["commands"]
    _require(isinstance(commands, list) and 0 < len(commands) <= 32, "invalid command list")
    manifest["commands"] = []
    for command in commands:
        _keys(command, {"path", "title"}, {"argsSchema", "views", "togglePanel"}, "command")
        path = command["path"]
        _require(isinstance(path, list) and 0 < len(path) <= 8 and
                 all(isinstance(token, str) and _COMMAND.fullmatch(token) for token in path),
                 "invalid command path")
        _require(tuple(path) not in paths, "duplicate command path")
        paths.add(tuple(path))
        generated = {"path": path, "title": _text(command["title"], 128, "command title")}
        if "argsSchema" in command:
            generated["argsSchema"] = add_file(command["argsSchema"], ".json")
        if "views" in command:
            views = _strings(command["views"], len(_VIEW), "command views")
            _require(set(views) <= _VIEW, "unknown command view")
            generated["views"] = views
        if "togglePanel" in command:
            toggle = command["togglePanel"]
            _require(isinstance(toggle, bool) and (not toggle or bool(generated.get("views"))),
                     "togglePanel requires a boolean and a declared view when enabled")
            generated["togglePanel"] = toggle
        manifest["commands"].append(generated)
    for path in _strings(declaration.get("files", []), 1024, "package members"):
        add_file(path)
    return manifest, sorted(members)
