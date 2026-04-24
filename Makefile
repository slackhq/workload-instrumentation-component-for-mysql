BUILD_TARGET     ?= plugin
MYSQL_FLAVOR     ?= oracle
MYSQL_VERSION    ?= 8.0.46
UBUNTU_VERSION   ?= 22.04
CMAKE_BUILD_TYPE ?= RelWithDebInfo

BUILD_IMAGE_NAME := workload-instrumentation-builder
BUILD_IMAGE_TAG  := $(MYSQL_FLAVOR)-$(MYSQL_VERSION)-ubuntu$(UBUNTU_VERSION)
BUILD_DOCKER_IMG := $(BUILD_IMAGE_NAME):$(BUILD_IMAGE_TAG)

TEST_IMAGE_NAME := workload-instrumentation-test
TEST_IMAGE_TAG  := $(MYSQL_FLAVOR)-$(MYSQL_VERSION)-ubuntu$(UBUNTU_VERSION)
TEST_DOCKER_IMG := $(TEST_IMAGE_NAME):$(TEST_IMAGE_TAG)

DIST_LABEL := $(MYSQL_FLAVOR)-$(MYSQL_VERSION)
DIST_DIR   := $(CURDIR)/dist/$(BUILD_TARGET)/$(DIST_LABEL)

ifeq ($(BUILD_TARGET),plugin)
  SO_NAME := workload_instrumentation.so
else ifeq ($(BUILD_TARGET),component)
  SO_NAME := component_workload_instrumentation.so
else
  $(error BUILD_TARGET must be 'plugin' or 'component')
endif

# ---------------------------------------------------------------------------
# build — compile and copy artifact to dist/<target>/<flavor>-<version>/
# ---------------------------------------------------------------------------
.PHONY: build
build: ensure-build-image
	mkdir -p $(DIST_DIR)
	docker run --rm \
		-v $(CURDIR)/plugin:/repo/plugin:ro \
		-v $(CURDIR)/component:/repo/component:ro \
		-v $(CURDIR)/scripts:/repo/scripts:ro \
		-v $(DIST_DIR):/dist \
		-e BUILD_TARGET=$(BUILD_TARGET) \
		-e CMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) \
		$(BUILD_DOCKER_IMG) \
		bash -c ' \
			scripts/build.sh && \
			cp $$MYSQL_BUILD/plugin_output_directory/$(SO_NAME) /dist/ \
		'
	@echo "Artifact: $(DIST_DIR)/$(SO_NAME)"

# ---------------------------------------------------------------------------
# ensure-build-image — build the docker image only if it doesn't already exist
# ---------------------------------------------------------------------------
.PHONY: ensure-build-image
ensure-build-image:
	@if ! docker image inspect $(BUILD_DOCKER_IMG) >/dev/null 2>&1; then \
		echo "Image $(BUILD_DOCKER_IMG) not found, building..."; \
		docker build -f build.Dockerfile \
			--build-arg UBUNTU_VERSION=$(UBUNTU_VERSION) \
			--build-arg MYSQL_FLAVOR=$(MYSQL_FLAVOR) \
			--build-arg MYSQL_VERSION=$(MYSQL_VERSION) \
			-t $(BUILD_DOCKER_IMG) .; \
	else \
		echo "Image $(BUILD_DOCKER_IMG) already exists, skipping build."; \
	fi

# ---------------------------------------------------------------------------
# rebuild-build-image — force rebuild the docker image
# ---------------------------------------------------------------------------
.PHONY: rebuild-build-image
rebuild-build-image:
	docker build -f build.Dockerfile \
		--build-arg UBUNTU_VERSION=$(UBUNTU_VERSION) \
		--build-arg MYSQL_FLAVOR=$(MYSQL_FLAVOR) \
		--build-arg MYSQL_VERSION=$(MYSQL_VERSION) \
		--no-cache \
		-t $(BUILD_DOCKER_IMG) .

# ---------------------------------------------------------------------------
# test — run integration tests inside a docker container
# ---------------------------------------------------------------------------
LOG_DIR := $(CURDIR)/logs

.PHONY: test
test: ensure-test-image
	mkdir -p $(LOG_DIR)
	docker run --rm \
		-v $(CURDIR)/scripts:/repo/scripts:ro \
		-v $(DIST_DIR):/repo/artifacts:ro \
		-v $(LOG_DIR):/repo/logs \
		-e BUILD_TARGET=$(BUILD_TARGET) \
		-e MYSQL_FLAVOR=$(MYSQL_FLAVOR) \
		-e MYSQL_VERSION=$(MYSQL_VERSION) \
		-e ARTIFACT_DIR=/repo/artifacts \
		-e LOG_DIR=/repo/logs \
		$(TEST_DOCKER_IMG) \
		bash /repo/scripts/test.sh

# ---------------------------------------------------------------------------
# ensure-test-image — build the test docker image only if it doesn't exist
# ---------------------------------------------------------------------------
.PHONY: ensure-test-image
ensure-test-image:
	@if ! docker image inspect $(TEST_DOCKER_IMG) >/dev/null 2>&1; then \
		echo "Image $(TEST_DOCKER_IMG) not found, building..."; \
		docker build -f test.Dockerfile \
			--build-arg UBUNTU_VERSION=$(UBUNTU_VERSION) \
			-t $(TEST_DOCKER_IMG) .; \
	else \
		echo "Image $(TEST_DOCKER_IMG) already exists, skipping build."; \
	fi

# ---------------------------------------------------------------------------
# rebuild-test-image — force rebuild the test docker image
# ---------------------------------------------------------------------------
.PHONY: rebuild-test-image
rebuild-test-image:
	docker build -f test.Dockerfile \
		--build-arg UBUNTU_VERSION=$(UBUNTU_VERSION) \
		--no-cache \
		-t $(TEST_DOCKER_IMG) .

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
.PHONY: clean
clean:
	rm -rf dist/

.PHONY: info
info:
	@echo "BUILD_TARGET      = $(BUILD_TARGET)"
	@echo "MYSQL_FLAVOR      = $(MYSQL_FLAVOR)"
	@echo "MYSQL_VERSION     = $(MYSQL_VERSION)"
	@echo "UBUNTU_VERSION    = $(UBUNTU_VERSION)"
	@echo "CMAKE_BUILD_TYPE  = $(CMAKE_BUILD_TYPE)"
	@echo "BUILD_DOCKER_IMG  = $(BUILD_DOCKER_IMG)"
	@echo "TEST_DOCKER_IMG   = $(TEST_DOCKER_IMG)"
	@echo "DIST_DIR          = $(DIST_DIR)"
	@echo "SO_NAME           = $(SO_NAME)"
