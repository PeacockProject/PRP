SHELL := /usr/bin/env bash

TARGET ?= jflte
CONFIG ?= $(CURDIR)/configs/$(TARGET).env
OUT_DIR ?= $(CURDIR)/out/$(TARGET)

.PHONY: help sync-assets check-kernel initramfs bootimg overlay gui-host gui-host-build run-gui-host backup-recovery flash-recovery clean

help:
	@echo "PRP standalone build system"
	@echo "Targets:"
	@echo "  make sync-assets TARGET=jflte"
	@echo "  make check-kernel TARGET=jflte"
	@echo "  make initramfs TARGET=jflte"
	@echo "  make bootimg TARGET=jflte"
	@echo "  make overlay TARGET=jflte"
	@echo "  make gui-host TARGET=jflte        (build + run host SDL simulator)"
	@echo "  make gui-host-build TARGET=jflte  (build only)"
	@echo "  make backup-recovery TARGET=jflte"
	@echo "  make flash-recovery TARGET=jflte"
	@echo "  make clean TARGET=jflte"
	@echo ""
	@echo "Override vars:"
	@echo "  CONFIG=/abs/path/to/device.env"
	@echo "  OUT_DIR=/abs/path/to/output"
	@echo "  GUI_HOST_RUN=0  (for gui-host: build only, don't launch)"

sync-assets:
	@mkdir -p "$(OUT_DIR)"
	@./scripts/sync-runtime-assets.sh "$(CONFIG)" "$(OUT_DIR)"

check-kernel:
	@mkdir -p "$(OUT_DIR)"
	@./scripts/check-kernel-config.sh "$(CONFIG)" "$(OUT_DIR)"

initramfs:
	@mkdir -p "$(OUT_DIR)"
	@./scripts/build-initramfs.sh "$(CONFIG)" "$(OUT_DIR)"

bootimg:
	@mkdir -p "$(OUT_DIR)"
	@./scripts/build-bootimg.sh "$(CONFIG)" "$(OUT_DIR)"

overlay:
	@mkdir -p "$(OUT_DIR)"
	@./scripts/build-overlay.sh "$(CONFIG)" "$(OUT_DIR)"

gui-host:
	@mkdir -p "$(OUT_DIR)"
	@./scripts/build-gui-host.sh "$(CONFIG)" "$(OUT_DIR)"
	@if [ "$${GUI_HOST_RUN:-1}" = "1" ]; then ./scripts/run-gui-host.sh "$(CONFIG)" "$(OUT_DIR)"; fi

gui-host-build:
	@mkdir -p "$(OUT_DIR)"
	@./scripts/build-gui-host.sh "$(CONFIG)" "$(OUT_DIR)"

run-gui-host:
	@mkdir -p "$(OUT_DIR)"
	@./scripts/run-gui-host.sh "$(CONFIG)" "$(OUT_DIR)"

backup-recovery:
	@mkdir -p "$(OUT_DIR)"
	@./scripts/backup-recovery.sh "$(CONFIG)" "$(OUT_DIR)"

flash-recovery:
	@mkdir -p "$(OUT_DIR)"
	@./scripts/flash-recovery.sh "$(CONFIG)" "$(OUT_DIR)"

clean:
	@rm -rf "$(OUT_DIR)"
	@echo "cleaned $(OUT_DIR)"
