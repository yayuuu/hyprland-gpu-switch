CXX ?= g++
PKG_CONFIG ?= pkg-config
DIAGNOSTICS ?= 0
PREFIX ?= $(HOME)/.local
LIBDIR ?= $(PREFIX)/lib/hyprland
DESTDIR ?=
CXXFLAGS ?= -O2
STANDARD_FLAGS = -std=c++23 -Wall -Wextra -Wno-missing-field-initializers

ifeq ($(DIAGNOSTICS),0)
PROFILE = release
else ifeq ($(DIAGNOSTICS),1)
PROFILE = diagnostics
STANDARD_FLAGS += -g
else
$(error DIAGNOSTICS must be 0 or 1)
endif

BUILD_DIR = .build/$(PROFILE)
PACKAGES = hyprland aquamarine hyprutils pixman-1 libdrm pangocairo libinput libudev wayland-server xkbcommon egl glesv2 vulkan
PACKAGE_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
PACKAGE_LIBS = $(shell $(PKG_CONFIG) --libs $(PACKAGES))
COMPILE_FLAGS = $(CPPFLAGS) -DGPU_SWITCH_DIAGNOSTICS=$(DIAGNOSTICS) $(CXXFLAGS) $(STANDARD_FLAGS)
SOURCES = rendering-optimizations.cpp client-transfer.cpp transfer/embedded.cpp transfer/worker-blob.S main.cpp probe.cpp migration.cpp drm-route.cpp aq-transfer.cpp native.cpp
ifeq ($(DIAGNOSTICS),1)
SOURCES += diagnostics/diagnostics.cpp
endif
HEADERS = $(wildcard *.hpp vendor/aquamarine/*.hpp transfer/*.hpp) diagnostics/diagnostics.hpp

.DEFAULT_GOAL := all
.PHONY: all check check-deps tools require-diagnostics install uninstall clean FORCE
all: hyprland-gpu-switch.so

check-deps:
	@$(PKG_CONFIG) --print-errors --exists $(PACKAGES)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/hyprland-gpu-switch.so: $(SOURCES) $(HEADERS) Makefile $(BUILD_DIR)/gpu-transfer-worker | check-deps $(BUILD_DIR)
	$(CXX) $(COMPILE_FLAGS) -DGPU_TRANSFER_WORKER_PATH='"$(BUILD_DIR)/gpu-transfer-worker"' -shared -fPIC -fno-gnu-unique $(PACKAGE_CFLAGS) $(SOURCES) $(LDFLAGS) $(PACKAGE_LIBS) -o $@.new
	mv $@.new $@

# Always select the requested profile, even when switching back to an older build.
hyprland-gpu-switch.so: $(BUILD_DIR)/hyprland-gpu-switch.so FORCE
	@if ! cmp -s "$<" "$@"; then cp "$<" "$@.new"; mv "$@.new" "$@"; fi

FORCE:

install: hyprland-gpu-switch.so
	install -d "$(DESTDIR)$(LIBDIR)"
	install -m 755 hyprland-gpu-switch.so "$(DESTDIR)$(LIBDIR)/hyprland-gpu-switch.so.new"
	mv "$(DESTDIR)$(LIBDIR)/hyprland-gpu-switch.so.new" "$(DESTDIR)$(LIBDIR)/hyprland-gpu-switch.so"

uninstall:
	rm -f "$(DESTDIR)$(LIBDIR)/hyprland-gpu-switch.so"

require-diagnostics:
	@test "$(DIAGNOSTICS)" = 1 || { echo 'Use make DIAGNOSTICS=1 tools for developer tools.' >&2; exit 1; }

tools: require-diagnostics $(BUILD_DIR)/gpu-switch-probe $(BUILD_DIR)/hook-probe.so $(BUILD_DIR)/transfer-opt.so

$(BUILD_DIR)/gpu-switch-probe: diagnostics/probe-main.cpp probe.cpp probe.hpp policy.hpp json.hpp Makefile | require-diagnostics $(BUILD_DIR)
	$(CXX) $(COMPILE_FLAGS) diagnostics/probe-main.cpp probe.cpp $(LDFLAGS) -lvulkan -o $@

$(BUILD_DIR)/hook-probe.so: diagnostics/hook-probe.cpp Makefile | require-diagnostics check-deps $(BUILD_DIR)
	$(CXX) $(COMPILE_FLAGS) -shared -fPIC $(PACKAGE_CFLAGS) diagnostics/hook-probe.cpp $(LDFLAGS) -o $@

$(BUILD_DIR)/transfer-opt.so: diagnostics/transfer-opt-plugin.cpp diagnostics/transfer-opt.cpp diagnostics/transfer-opt.hpp native.cpp native.hpp Makefile | require-diagnostics check-deps $(BUILD_DIR)
	$(CXX) $(COMPILE_FLAGS) -shared -fPIC -fno-gnu-unique $(PACKAGE_CFLAGS) diagnostics/transfer-opt-plugin.cpp diagnostics/transfer-opt.cpp native.cpp $(LDFLAGS) -laquamarine -lEGL -lGLESv2 -o $@.new
	mv $@.new $@

# Tests are opt-in and kept alongside the selected build profile.
.PHONY: policy-test buffer-compat-test route-plan-test native-test transfer-test
policy-test: $(BUILD_DIR)/policy-test
buffer-compat-test: $(BUILD_DIR)/buffer-compat-test
route-plan-test: $(BUILD_DIR)/route-plan-test
native-test: $(BUILD_DIR)/native-test
transfer-test: $(BUILD_DIR)/transfer-test

$(BUILD_DIR)/policy-test: tests/policy-test.cpp policy.hpp Makefile | $(BUILD_DIR)
	$(CXX) $(COMPILE_FLAGS) -I. tests/policy-test.cpp -o $@

$(BUILD_DIR)/buffer-compat-test: tests/buffer-compat-test.cpp buffer-compat.hpp Makefile | $(BUILD_DIR)
	$(CXX) $(COMPILE_FLAGS) -I. tests/buffer-compat-test.cpp -o $@

$(BUILD_DIR)/route-plan-test: tests/route-plan-test.cpp route-plan.hpp Makefile | $(BUILD_DIR)
	$(CXX) $(COMPILE_FLAGS) -I. tests/route-plan-test.cpp -o $@

$(BUILD_DIR)/diagnostics-test: tests/diagnostics-test.cpp diagnostics/diagnostics.cpp diagnostics/diagnostics.hpp Makefile | $(BUILD_DIR)
	$(CXX) $(COMPILE_FLAGS) -I. tests/diagnostics-test.cpp diagnostics/diagnostics.cpp $(LDFLAGS) -lEGL -o $@

check: $(BUILD_DIR)/policy-test $(BUILD_DIR)/buffer-compat-test $(BUILD_DIR)/route-plan-test $(BUILD_DIR)/diagnostics-test
	./$(BUILD_DIR)/policy-test
	./$(BUILD_DIR)/buffer-compat-test
	./$(BUILD_DIR)/route-plan-test
	./$(BUILD_DIR)/diagnostics-test

$(BUILD_DIR)/native-test: tests/native-test.cpp native.cpp native.hpp Makefile | $(BUILD_DIR)
	$(CXX) $(COMPILE_FLAGS) -I. tests/native-test.cpp native.cpp $(LDFLAGS) -lEGL -lGLESv2 -o $@

$(BUILD_DIR)/transfer-test: tests/transfer-test.cpp aq-transfer.cpp diagnostics/transfer-opt.cpp diagnostics/transfer-opt.hpp native.cpp $(HEADERS) Makefile | $(BUILD_DIR)
	$(CXX) $(COMPILE_FLAGS) -I. $(shell $(PKG_CONFIG) --cflags pixman-1 libdrm) tests/transfer-test.cpp aq-transfer.cpp diagnostics/transfer-opt.cpp native.cpp $(LDFLAGS) -laquamarine -lhyprutils -lEGL -lGLESv2 -o $@

clean:
	rm -rf .build
	rm -f hyprland-gpu-switch.so hyprland-gpu-switch.so.new gpu-switch-probe hook-probe.so transfer-opt.so transfer-opt.so.new policy-test buffer-compat-test route-plan-test native-test transfer-test

.build/transfer:
	mkdir -p $@

.build/transfer/pq.comp.spv: transfer/pq.comp | .build/transfer
	glslangValidator -V $< -o $@

$(BUILD_DIR)/gpu-transfer-worker: transfer/worker.cpp transfer/protocol.hpp transfer/formats.hpp transfer/supervisor.hpp transfer/shader-blob.S .build/transfer/pq.comp.spv Makefile | $(BUILD_DIR)
	$(CXX) -DGPU_SWITCH_DIAGNOSTICS=$(DIAGNOSTICS) -O2 -std=c++23 -Wall -Wextra -Werror -Wno-missing-field-initializers transfer/worker.cpp transfer/shader-blob.S -lvulkan -o $@.new
	strip --strip-debug $@.new
	mv $@.new $@

$(BUILD_DIR)/gpu-route-test: tests/gpu-route-test.cpp transfer/gpu_route.hpp | $(BUILD_DIR)
	$(CXX) $(COMPILE_FLAGS) $< -o $@

$(BUILD_DIR)/queued-state-test: tests/queued-state-test.cpp transfer/queued_state.hpp | $(BUILD_DIR)
	$(CXX) $(COMPILE_FLAGS) $< -lhyprutils -o $@

$(BUILD_DIR)/cross-gpu-worker-test: tests/cross-gpu-worker-test.cpp $(wildcard transfer/*.hpp) | $(BUILD_DIR)
	$(CXX) $(COMPILE_FLAGS) $< -lvulkan -lEGL -lGLESv2 -o $@

.PHONY: gpu-transfer-test transport-check
transport-check: $(BUILD_DIR)/gpu-route-test $(BUILD_DIR)/queued-state-test
	./$(BUILD_DIR)/gpu-route-test
	./$(BUILD_DIR)/queued-state-test

gpu-transfer-test: $(BUILD_DIR)/cross-gpu-worker-test $(BUILD_DIR)/gpu-transfer-worker
check: transport-check

$(BUILD_DIR)/route-lifetime-test: tests/route-lifetime-test.cpp route-lifetime.hpp | $(BUILD_DIR)
	$(CXX) $(COMPILE_FLAGS) -I. $< -lhyprutils -o $@

.PHONY: route-lifetime-check
route-lifetime-check: $(BUILD_DIR)/route-lifetime-test
	./$(BUILD_DIR)/route-lifetime-test
check: route-lifetime-check

$(BUILD_DIR)/rendering-policy-test: tests/rendering-policy-test.cpp singlepass-policy.hpp capture-wait.hpp | $(BUILD_DIR)
	$(CXX) $(COMPILE_FLAGS) -I. $< -lhyprutils -o $@

.PHONY: rendering-policy-check
rendering-policy-check: $(BUILD_DIR)/rendering-policy-test
	./$(BUILD_DIR)/rendering-policy-test
check: rendering-policy-check

$(BUILD_DIR)/frame-cadence-probe.so: diagnostics/frame-cadence.cpp transfer/queued_state.hpp Makefile | require-diagnostics check-deps $(BUILD_DIR)
	$(CXX) $(COMPILE_FLAGS) -shared -fPIC -fno-gnu-unique $(PACKAGE_CFLAGS) $< -o $@
tools: $(BUILD_DIR)/frame-cadence-probe.so

$(BUILD_DIR)/session-pool-test: tests/session-pool-test.cpp transfer/session-pool.hpp | $(BUILD_DIR)
	$(CXX) $(COMPILE_FLAGS) $< -o $@

.PHONY: session-pool-check
session-pool-check: $(BUILD_DIR)/session-pool-test
	./$(BUILD_DIR)/session-pool-test
check: session-pool-check

$(BUILD_DIR)/idle-policy-test: tests/idle-policy-test.cpp transfer/idle-policy.hpp | $(BUILD_DIR)
	$(CXX) $(COMPILE_FLAGS) $< -o $@

.PHONY: idle-policy-check
idle-policy-check: $(BUILD_DIR)/idle-policy-test
	./$(BUILD_DIR)/idle-policy-test
check: idle-policy-check

$(BUILD_DIR)/gl-cache-repair.so: diagnostics/gl-cache-repair.cpp gl-state-cache.hpp Makefile | require-diagnostics check-deps $(BUILD_DIR)
	$(CXX) $(COMPILE_FLAGS) -shared -fPIC -fno-gnu-unique $(PACKAGE_CFLAGS) $< -lEGL -lGLESv2 -o $@

$(BUILD_DIR)/gl-state-cache-test: tests/gl-state-cache-test.cpp gl-state-cache.hpp native.cpp native.hpp | $(BUILD_DIR)
	$(CXX) $(COMPILE_FLAGS) -I. $< native.cpp -lEGL -lGLESv2 -o $@

$(BUILD_DIR)/transfer-timers.so: diagnostics/transfer-timers.cpp Makefile | require-diagnostics check-deps $(BUILD_DIR)
	$(CXX) $(COMPILE_FLAGS) -shared -fPIC $(PACKAGE_CFLAGS) $< -o $@
tools: $(BUILD_DIR)/transfer-timers.so

$(BUILD_DIR)/retained-source-test: tests/retained-source-test.cpp transfer/retained-source.hpp transfer/protocol.hpp | $(BUILD_DIR)
	$(CXX) $(COMPILE_FLAGS) -I. $(shell $(PKG_CONFIG) --cflags aquamarine pixman-1) $< -lhyprutils -o $@
.PHONY: retained-source-check
retained-source-check: $(BUILD_DIR)/retained-source-test
	./$(BUILD_DIR)/retained-source-test
check: retained-source-check

$(BUILD_DIR)/output-wake-probe.so: diagnostics/output-wake-probe.cpp json.hpp Makefile | require-diagnostics check-deps $(BUILD_DIR)
	$(CXX) $(COMPILE_FLAGS) -shared -fPIC -fno-gnu-unique $(PACKAGE_CFLAGS) $< -o $@.new
	mv $@.new $@

$(BUILD_DIR)/route-resources-test: tests/route-resources-test.cpp route-resources.hpp | $(BUILD_DIR)
	$(CXX) $(COMPILE_FLAGS) -I. $< -o $@
