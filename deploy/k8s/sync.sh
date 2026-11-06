#!/bin/sh
# deploy/prometheus/ and deploy/grafana/ are the sources of truth. This copies them into the central
# kustomization (kustomize cannot read files outside its directory) and regenerates the
# PrometheusRule of the helm variant. Run it after editing rules or dashboards and commit the
# result; CI runs it and fails if the committed copies are stale.
set -eu
cd "$(dirname "$0")/../.."
FILES=deploy/k8s/central/kustomize/plain/files
rm -rf "$FILES"
mkdir -p "$FILES/rules" "$FILES/grafana/datasources" "$FILES/grafana/providers" "$FILES/grafana/dashboards"
cp deploy/prometheus/prometheus.yml deploy/prometheus/alertmanager.yml "$FILES/"
cp deploy/prometheus/rules/ros2.yaml "$FILES/rules/"
cp deploy/grafana/provisioning/datasources/prometheus.yaml "$FILES/grafana/datasources/"
cp deploy/grafana/provisioning/dashboards/ros2ebpf.yaml "$FILES/grafana/providers/"
cp deploy/grafana/dashboards/*.json "$FILES/grafana/dashboards/"
python3 - <<'PY'
import yaml
rules = yaml.safe_load(open("deploy/prometheus/rules/ros2.yaml"))
doc = {"apiVersion": "monitoring.coreos.com/v1", "kind": "PrometheusRule",
       "metadata": {"name": "ros2ebpf", "namespace": "observability", "labels": {"release": "kps"}},
       "spec": rules}
with open("deploy/k8s/central/helm/prometheusrule-ros2.yaml", "w") as f:
    f.write("# Generated from deploy/prometheus/rules/ros2.yaml (deploy/k8s/sync.sh)\n")
    yaml.safe_dump(doc, f, sort_keys=False, width=200)
PY
echo "deploy/k8s: synced"
