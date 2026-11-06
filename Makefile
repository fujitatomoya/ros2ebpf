# Convenience targets around colcon. Everything also works with plain colcon / docker; see docs/development.md.
BUILD ?= build
JOBS  ?= $(shell nproc)
ROS_DISTRO ?= rolling
ROS_SETUP  ?= /opt/ros/$(ROS_DISTRO)/setup.sh
IMAGE_BUILD ?= ros2ebpf-build:local
IMAGE_CI ?= tomoyafujita/ros2dev:$(ROS_DISTRO)   # ROS 2 source-build environment used by CI
# run as the host user so build/ install/ log/ stay owned by you
DOCKER_RUN = docker run --rm -u $(shell id -u):$(shell id -g) -e HOME=/tmp -v "$(CURDIR)":/src -w /src $(IMAGE_BUILD)

# source the ROS underlay for every colcon call (a no-op if the shell already did)
COLCON = . $(ROS_SETUP) && colcon
C_PKGS = ros2ebpf_exporter ros2ebpf_daemon ros2ebpf_interfaces
CLI_PKG = ros2ebpf
BUILD_ARGS = --merge-install --build-base $(BUILD) --event-handlers console_cohesion+
BUILD_TYPE ?= RelWithDebInfo

.PHONY: all cli test cli-test clean deb in-docker-deb \
        build-image in-docker-test in-docker-ci images image-daemon image-cli image-demo e2e e2e-clean k8s-sync k8s-rules lint

all:                             ## exporter + daemon + interfaces -> install/ (merged prefix)
	$(COLCON) build $(BUILD_ARGS) --packages-up-to $(C_PKGS) --cmake-args -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

cli:                             ## the `ros2 ebpf` CLI (ament_python)
	$(COLCON) build $(BUILD_ARGS) --packages-select $(CLI_PKG)

test: all                        ## C unit tests (ament_add_test -> ctest)
	$(COLCON) test $(BUILD_ARGS) --packages-select $(C_PKGS)
	$(COLCON) test-result --test-result-base $(BUILD) --verbose

cli-test: cli                    ## pytest for the CLI + flake8
	$(COLCON) test $(BUILD_ARGS) --packages-select $(CLI_PKG)
	$(COLCON) test-result --test-result-base $(BUILD) --verbose
	cd ros2ebpf && python3 -m flake8 --select=E,W,F --extend-ignore=W503 --max-line-length=120 ros2ebpf test

clean:
	rm -rf $(BUILD) install log

# The deb build uses plain cmake in its own build dirs; colcon marks its own build base itself.
$(BUILD)/COLCON_IGNORE:
	mkdir -p $(BUILD) && touch $@

deb: $(BUILD)/COLCON_IGNORE      ## Debian packages into build/pkg (run natively or via in-docker-deb)
	. $(ROS_SETUP) && cmake -S ros2ebpf_exporter -B $(BUILD)/pkg-exporter -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF
	cmake --build $(BUILD)/pkg-exporter -j$(JOBS)
	cd $(BUILD)/pkg-exporter && cpack -G DEB
	cmake --install $(BUILD)/pkg-exporter --prefix $(abspath $(BUILD))/pkg-stage
	. $(ROS_SETUP) && cmake -S ros2ebpf_daemon -B $(BUILD)/pkg-daemon -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF \
	    -DCMAKE_PREFIX_PATH=$(abspath $(BUILD))/pkg-stage
	cmake --build $(BUILD)/pkg-daemon -j$(JOBS)
	cd $(BUILD)/pkg-daemon && cpack -G DEB
	mkdir -p $(BUILD)/pkg && cp $(BUILD)/pkg-exporter/*.deb $(BUILD)/pkg-daemon/*.deb $(BUILD)/pkg/
	ls -l $(BUILD)/pkg

in-docker-deb: build-image
	$(DOCKER_RUN) make deb

# ---- containers ------------------------------------------------------------
build-image:                     ## toolchain image used by `make in-docker-*` (ros-base + clang/libbpf/bpftool)
	docker build --build-arg ROS_DISTRO=$(ROS_DISTRO) -t $(IMAGE_BUILD) -f docker/Dockerfile.build docker

in-docker-test: build-image      ## colcon build + tests without installing ROS/clang/libbpf on the host
	$(DOCKER_RUN) make test cli-test

in-docker-ci:                    ## exactly what CI runs: scripts/github_workflows.sh in the rolling source-build image (privileged: BPF load)
	docker run --rm --privileged -v "$(CURDIR)":/src:ro -w /src -e ROS_DISTRO=$(ROS_DISTRO) $(IMAGE_CI) bash ./scripts/github_workflows.sh

image-daemon:
	docker build --build-arg ROS_DISTRO=$(ROS_DISTRO) -f docker/Dockerfile.daemon -t ros2ebpfd:local .
image-cli:
	docker build --build-arg ROS_DISTRO=$(ROS_DISTRO) -f docker/Dockerfile.cli -t ros2ebpf-cli:local .
image-demo:
	docker build --build-arg ROS_DISTRO=$(ROS_DISTRO) -f docker/Dockerfile.demo -t ros2ebpf-demo:local .
images: image-daemon image-cli image-demo

e2e: images                      ## demo nodes in one container, daemon in another, assertions on /metrics
	./scripts/e2e.sh

e2e-clean:
	docker rm -f r2e-demo r2e-daemon 2>/dev/null || true

# ---- deploy ----------------------------------------------------------------
K8S_FILES = deploy/k8s/central/kustomize/plain/files
k8s-sync: k8s-rules              ## copy prometheus/grafana sources into the central kustomization (kustomize load restriction)
	rm -rf $(K8S_FILES) && mkdir -p $(K8S_FILES)/rules $(K8S_FILES)/grafana/datasources $(K8S_FILES)/grafana/providers $(K8S_FILES)/grafana/dashboards
	cp deploy/prometheus/prometheus.yml deploy/prometheus/alertmanager.yml $(K8S_FILES)/
	cp deploy/prometheus/rules/ros2.yaml $(K8S_FILES)/rules/
	cp deploy/grafana/provisioning/datasources/prometheus.yaml $(K8S_FILES)/grafana/datasources/
	cp deploy/grafana/provisioning/dashboards/ros2ebpf.yaml $(K8S_FILES)/grafana/providers/
	cp deploy/grafana/dashboards/*.json $(K8S_FILES)/grafana/dashboards/

k8s-rules:                       ## regenerate the PrometheusRule from deploy/prometheus/rules/ros2.yaml
	python3 -c 'import yaml;r=yaml.safe_load(open("deploy/prometheus/rules/ros2.yaml"));d={"apiVersion":"monitoring.coreos.com/v1","kind":"PrometheusRule","metadata":{"name":"ros2ebpf","namespace":"observability","labels":{"release":"kps"}},"spec":r};f=open("deploy/k8s/central/helm/prometheusrule-ros2.yaml","w");f.write("# Generated from deploy/prometheus/rules/ros2.yaml (make k8s-rules)\n");yaml.safe_dump(d,f,sort_keys=False,width=200)'

lint: k8s-sync                   ## validate rules, alertmanager config, kustomizations, compose file
	docker run --rm -v "$(CURDIR)/deploy/prometheus:/p:ro" --entrypoint promtool prom/prometheus:v3.5.0 check rules /p/rules/ros2.yaml
	docker run --rm -v "$(CURDIR)/deploy/prometheus:/p:ro" --entrypoint amtool prom/alertmanager:v0.28.1 check-config /p/alertmanager.yml
	docker run --rm -v "$(CURDIR)/deploy:/d:ro" registry.k8s.io/kustomize/kustomize:v5.4.3 build /d/k8s/robot > /dev/null
	docker run --rm -v "$(CURDIR)/deploy:/d:ro" registry.k8s.io/kustomize/kustomize:v5.4.3 build /d/k8s/central/kustomize/plain > /dev/null
	docker compose -f deploy/docker-compose/compose.yaml config > /dev/null
