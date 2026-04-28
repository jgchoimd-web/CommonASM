from dataclasses import dataclass, field


@dataclass(frozen=True)
class Constant:
    name: str
    value: int


@dataclass(frozen=True)
class DataString:
    name: str
    value: str


@dataclass(frozen=True)
class DataBytes:
    name: str
    values: list[int]


@dataclass(frozen=True)
class Label:
    name: str


@dataclass(frozen=True)
class Global:
    name: str


@dataclass(frozen=True)
class Instruction:
    op: str
    args: list[str]
    line: int


DataItem = DataString | DataBytes
TextItem = Label | Global | Instruction


@dataclass
class Program:
    constants: list[Constant] = field(default_factory=list)
    data: list[DataItem] = field(default_factory=list)
    text: list[TextItem] = field(default_factory=list)
