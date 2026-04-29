CC ?= gcc
CFLAGS ?= -std=c99 -Wall -Wextra -pedantic -O2
BUILD_DIR ?= build
COMMONASMC ?= $(BUILD_DIR)/commonasmc
POWERSHELL ?= pwsh

.PHONY: help build smoke smoke-sh smoke-ps examples clean

help:
	@echo "CommonASM developer targets:"
	@echo "  make build      Build the C AOT compiler"
	@echo "  make smoke      Run the POSIX smoke test suite"
	@echo "  make smoke-sh   Run scripts/smoke-test.sh"
	@echo "  make smoke-ps   Run scripts/smoke-test.ps1 with PowerShell"
	@echo "  make examples   Compile a few representative examples"
	@echo "  make clean      Remove build outputs"

build:
	mkdir -p "$(BUILD_DIR)"
	$(CC) $(CFLAGS) csrc/commonasmc.c -o "$(COMMONASMC)"

smoke: smoke-sh

smoke-sh:
	sh scripts/smoke-test.sh

smoke-ps:
	$(POWERSHELL) -NoProfile -File scripts/smoke-test.ps1

examples: build
	"$(COMMONASMC)" examples/hello.cas --target x86_64-nasm -o "$(BUILD_DIR)/hello_x86.asm"
	"$(COMMONASMC)" examples/hello.cas --target riscv64-gnu -o "$(BUILD_DIR)/hello_rv64.s"
	"$(COMMONASMC)" examples/vm_ir.cas --target wasm -o "$(BUILD_DIR)/vm.wat"
	"$(COMMONASMC)" examples/retro_toy.cas --target brainfuck -o "$(BUILD_DIR)/retro.bf"

clean:
	rm -rf "$(BUILD_DIR)"
