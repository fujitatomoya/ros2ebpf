# Convenience targets. Everything also works with plain cmake / docker; see docs/development.md.
BUILD ?= build
JOBS  ?= $(shell nproc)
IMAGE_BUILD ?= ros2ebpf-build:local

.PHONY: all exporter daemon test cli-test install clean \
        build-image images image-daemon image-cli image-demo e2e e2e-clean k8s-rules lint

all: exporter daemon

# The build tree holds staged copies of package.xml (cpack); mark it so colcon skips it when
# this checkout sits inside a colcon workspace.
$(BUILD)/COLCON_IGNORE:
	mkdir -p $(BUILD) && touch $@

exporter: $(BUILD)/COLCON_IGNORE
	cmake -S ros2ebpf_exporter -B $(BUILD)/exporter -DCMAKE_BUILD_TYPE=RelWithDebInfo
	cmake --build $(BUILD)/exporter -j$(JOBS)

daemon: exporter
	cmake -S ros2ebpf_daemon -B $(BUILD)/daemon -DCMAKE_BUILD_TYPE=RelWithDebInfo
	cmake --build $(BUILD)/daemon -j$(JOBS)

test: all
	ctest --test-dir $(BUILD)/exporter --output-on-failure
	ctest --test-dir $(BUILD)/daemon --output-on-failure

cli-test:
	cd ros2ebpf && python3 -m pytest -q test

install: all
	cmake --install $(BUILD)/exporter
	cmake --install $(BUILD)/daemon

clean:
	rm -rf $(BUILD)

deb: $(BUILD)/COLCON_IGNORE      ## Debian packages into build/pkg (run natively or via in-docker-deb)
	cmake -S ros2ebpf_exporter -B $(BUILD)/pkg-exporter -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DROS2EBPF_BUILD_TESTING=OFF
	cmake --build $(BUILD)/pkg-exporter -j$(JOBS)
	cd $(BUILD)/pkg-exporter && cpack -G DEB
	cmake -S ros2ebpf_daemon -B $(BUILD)/pkg-daemon -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DROS2EBPF_DAEMON_TESTING=OFF
	cmake --build $(BUILD)/pkg-daemon -j$(JOBS)
	cd $(BUILD)/pkg-daemon && cpack -G DEB
	mkdir -p $(BUILD)/pkg && cp $(BUILD)/pkg-exporter/*.deb $(BUILD)/pkg-daemon/*.deb $(BUILD)/pkg/
	ls -l $(BUILD)/pkg

in-docker-deb: build-image
	docker run --rm -v "$(CURDIR)":/src -w /src $(IMAGE_BUILD) make deb

# ---- containers ------------------------------------------------------------
build-image:                     ## toolchain image used by `make in-docker-*`
	docker build -t $(IMAGE_BUILD) -f docker/Dockerfile.build docker

in-docker-test: build-image      ## build + unit tests without installing clang/libbpf on the host
	docker run --rm -v "$(CURDIR)":/src -w /src $(IMAGE_BUILD) make test cli-test

image-daemon:
	docker build -f docker/Dockerfile.daemon -t ros2ebpfd:local .
image-cli:
	docker build -f docker/Dockerfile.cli -t ros2ebpf-cli:local .
image-demo:
	docker build -f docker/Dockerfile.demo -t ros2ebpf-demo:local .
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
