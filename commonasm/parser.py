import ast
import re

from .ir import Constant, DataBytes, DataString, Global, Instruction, Label, Program


_CONST_RE = re.compile(r"^const\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(-?(?:0x[0-9A-Fa-f]+|\d+))$")
_DATA_STRING_RE = re.compile(r'^([A-Za-z_][A-Za-z0-9_]*):\s*string\s+(".*")$')
_DATA_BYTES_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*):\s*bytes\s+(.+)$")
_LABEL_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*):$")


class ParseError(Exception):
    pass


def parse(source: str) -> Program:
    program = Program()
    section: str | None = None

    for line_no, raw_line in enumerate(source.splitlines(), start=1):
        line = _strip_comment(raw_line).strip()
        if not line:
            continue

        if line in {".data", ".text"}:
            section = line[1:]
            continue

        constant = _parse_const(line)
        if constant:
            program.constants.append(constant)
            continue

        if section is None:
            raise ParseError(f"line {line_no}: expected .data or .text before code")

        if section == "data":
            program.data.append(_parse_data(line, line_no))
        else:
            item = _parse_text(line, line_no)
            program.text.append(item)

    return program


def _strip_comment(line: str) -> str:
    in_string = False
    escaped = False
    for index, char in enumerate(line):
        if escaped:
            escaped = False
            continue
        if char == "\\" and in_string:
            escaped = True
            continue
        if char == '"':
            in_string = not in_string
            continue
        if not in_string and char in {";", "#"}:
            return line[:index]
    return line


def _parse_const(line: str) -> Constant | None:
    match = _CONST_RE.match(line)
    if not match:
        return None
    name, raw_value = match.groups()
    return Constant(name=name, value=int(raw_value, 0))


def _parse_data(line: str, line_no: int) -> DataString | DataBytes:
    string_match = _DATA_STRING_RE.match(line)
    if string_match:
        name, raw_value = string_match.groups()
        return DataString(name=name, value=_parse_string(raw_value, line_no))

    bytes_match = _DATA_BYTES_RE.match(line)
    if bytes_match:
        name, raw_values = bytes_match.groups()
        values: list[int] = []
        for raw_value in raw_values.split(","):
            raw_value = raw_value.strip()
            try:
                value = int(raw_value, 0)
            except ValueError as exc:
                raise ParseError(f"line {line_no}: invalid byte value: {raw_value}") from exc
            if not 0 <= value <= 255:
                raise ParseError(f"line {line_no}: byte value must be 0-255: {raw_value}")
            values.append(value)
        if not values:
            raise ParseError(f"line {line_no}: bytes data needs at least one value")
        return DataBytes(name=name, values=values)

    raise ParseError(f"line {line_no}: expected data like: name: string \"text\" or name: bytes 1, 2")


def _parse_string(raw_value: str, line_no: int) -> str:
    try:
        value = ast.literal_eval(raw_value)
    except (SyntaxError, ValueError) as exc:
        raise ParseError(f"line {line_no}: invalid string literal") from exc
    if not isinstance(value, str):
        raise ParseError(f"line {line_no}: string data must be a string literal")
    return value


def _parse_text(line: str, line_no: int) -> Label | Global | Instruction:
    label = _LABEL_RE.match(line)
    if label:
        return Label(label.group(1))

    if line.startswith("global "):
        name = line.removeprefix("global ").strip()
        if not name:
            raise ParseError(f"line {line_no}: global needs a symbol name")
        return Global(name)

    if " " in line:
        op, arg_text = line.split(None, 1)
        args = [part.strip() for part in arg_text.split(",") if part.strip()]
    else:
        op, args = line, []
    return Instruction(op=op, args=args, line=line_no)
