BUILD_TARGET     ?= plugin
MYSQL_FLAVOR     ?= oracle
MYSQL_VERSION    ?= 8.0.42
UBUNTU_VERSION   ?= 22.04
CMAKE_BUILD_TYPE ?= RelWithDebInfo

IMAGE_NAME := workload-instrumentation-builder
IMAGE_TAG  := $(MYSQL_FLAVOR)-$(MYSQL_VERSION)-ubuntu$(UBUNTU_VERSION)
DOCKER_IMG := $(IMAGE_NAME):$(IMAGE_TAG)

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
build: ensure-image
	mkdir -p $(DIST_DIR)
	docker run --rm \
		-v $(CURDIR)/plugin:/repo/plugin:ro \
		-v $(CURDIR)/component:/repo/component:ro \
		-v $(CURDIR)/scripts:/repo/scripts:ro \
		-v $(DIST_DIR):/dist \
		-e BUILD_TARGET=$(BUILD_TARGET) \
		-e CMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) \
		$(DOCKER_IMG) \
		bash -c ' \
			scripts/build.sh && \
			cp $$MYSQL_BUILD/plugin_output_directory/$(SO_NAME) /dist/ \
		'
	@echo "Artifact: $(DIST_DIR)/$(SO_NAME)"

# ---------------------------------------------------------------------------
# ensure-image — build the docker image only if it doesn't already exist
# ---------------------------------------------------------------------------
.PHONY: ensure-image
ensure-image:
	@if ! docker image inspect $(DOCKER_IMG) >/dev/null 2>&1; then \
		echo "Image $(DOCKER_IMG) not found, building..."; \
		docker build -f build.Dockerfile \
			--build-arg UBUNTU_VERSION=$(UBUNTU_VERSION) \
			--build-arg MYSQL_FLAVOR=$(MYSQL_FLAVOR) \
			--build-arg MYSQL_VERSION=$(MYSQL_VERSION) \
			-t $(DOCKER_IMG) .; \
	else \
		echo "Image $(DOCKER_IMG) already exists, skipping build."; \
	fi

# ---------------------------------------------------------------------------
# rebuild-image — force rebuild the docker image
# ---------------------------------------------------------------------------
.PHONY: rebuild-image
rebuild-image:
	docker build -f build.Dockerfile \
		--build-arg UBUNTU_VERSION=$(UBUNTU_VERSION) \
		--build-arg MYSQL_FLAVOR=$(MYSQL_FLAVOR) \
		--build-arg MYSQL_VERSION=$(MYSQL_VERSION) \
		--no-cache \
		-t $(DOCKER_IMG) .

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
.PHONY: clean
clean:
	rm -rf dist/

.PHONY: info
info:
	@echo "BUILD_TARGET     = $(BUILD_TARGET)"
	@echo "MYSQL_FLAVOR     = $(MYSQL_FLAVOR)"
	@echo "MYSQL_VERSION    = $(MYSQL_VERSION)"
	@echo "UBUNTU_VERSION   = $(UBUNTU_VERSION)"
	@echo "CMAKE_BUILD_TYPE = $(CMAKE_BUILD_TYPE)"
	@echo "DOCKER_IMG       = $(DOCKER_IMG)"
	@echo "DIST_DIR         = $(DIST_DIR)"
	@echo "SO_NAME          = $(SO_NAME)"
