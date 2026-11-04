# ros2ebpf_interfaces: control protocol

The `ros2 ebpf` CLI, `ros2ebpf-ctl` and any other client talk to `ros2ebpfd`
with **JSON-RPC 2.0, one JSON object per line**, over:

| Transport | Where | Notes |
|---|---|---|
| unix socket | `/run/ros2ebpf/control.sock` (config `control.socket`) | mode 0660, group `ros2ebpf` if that group exists; otherwise root only |
| ssh | `ssh <host> ros2ebpf-ctl rpc` | the same line protocol over the ssh pipes; no daemon-side network listener |
| http | `POST /rpc` on `control.http` (e.g. `0.0.0.0:9478`), `GET /status` | off by default; **no authentication**, firewall it to the robot's LAN |

A request:

```json
{"jsonrpc":"2.0","id":1,"method":"enable","params":{"family":"take"}}
```

A response:

```json
{"jsonrpc":"2.0","id":1,"result":{"family":"take","enabled":true,"attached":2,"expected":2}}
```

Errors use the JSON-RPC error object with codes `-32700` (parse), `-32600`
(invalid request), `-32601` (unknown method), `-32602` (bad params),
`-32000` (daemon-side failure, message explains).

## Methods

| Method | Params | Result |
|---|---|---|
| `status` | none | see `status.schema.json`; `healthy` is true when every configured family is fully attached and the exporter listener is up |
| `enable` | `{family}` with `publish`, `take`, `services` or `udp_drops` | `{family, enabled, attached, expected[, warning]}`; counters are kept |
| `disable` | `{family}` | same |
| `reload` | none | `{message}`; re-reads config (filters, probes, buckets, expectations, log level); keys marked *restart* in `config.example.yaml` are ignored with a note in `message` |
| `snapshot` | none | `{ts_ns, endpoints: [{pid, handle, kind, name, rmw, flags, count, count2, seq_gaps, seq_lost, period_sum_ns, comm, domain_id}]}`; `ts_ns` is CLOCK_MONOTONIC. `count`/`count2` semantics per kind: publisher: publishes / 0; subscription: takes / 0; client: requests sent / responses taken; service: requests taken / responses sent |
| `config` | none | effective configuration (same keys as `config.yaml`) plus `path` |
| `metrics` | none | `{text}`: the Prometheus exposition the exporter would serve |
| `discover` | none | `{new_shims}`: run one shim discovery pass now |

The schemas in this directory (`request.schema.json`, `status.schema.json`)
are the contract the CLI tests against. Add fields freely; never rename or
change the type of an existing one without bumping `version` in `status`.
