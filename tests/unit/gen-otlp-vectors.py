#!/usr/bin/env python3
"""Writes tests/unit/otlp_vectors.h: an ExportMetricsServiceRequest built with
OpenTelemetry's own protobuf classes, which test_metrics.c compares with what
fc_metrics_encode writes for the same points. Run with a python that has
opentelemetry-proto (pip install opentelemetry-proto):

    python3 tests/unit/gen-otlp-vectors.py
"""
import os

from opentelemetry.proto.collector.metrics.v1 import metrics_service_pb2 as ms
from opentelemetry.proto.common.v1 import common_pb2 as c
from opentelemetry.proto.metrics.v1 import metrics_pb2 as m

NOW = 1800000000123456789
VERSION = "v3.13"


def attr(kvs, key, value):
    kvs.add(key=key, value=c.AnyValue(string_value=value))


req = ms.ExportMetricsServiceRequest()
rm = req.resource_metrics.add()
attr(rm.resource.attributes, "service.name", "flightctl-mcu")
attr(rm.resource.attributes, "service.version", VERSION)
sm = rm.scope_metrics.add()
sm.scope.name = "flightctl-mcu"
sm.scope.version = VERSION

g = sm.metrics.add(name="device.memory.free", unit="By")
g.gauge.data_points.add(time_unix_nano=NOW, as_int=123456)

s = sm.metrics.add(name="device.network.io", unit="By")
s.sum.aggregation_temporality = m.AGGREGATION_TEMPORALITY_CUMULATIVE
s.sum.is_monotonic = True
dp = s.sum.data_points.add(start_time_unix_nano=1700000000000000000, time_unix_nano=NOW, as_int=9790)
attr(dp.attributes, "direction", "transmit")

g = sm.metrics.add(name="device.cpu.utilization", unit="%")
g.gauge.data_points.add(time_unix_nano=NOW, as_double=12.5)

s = sm.metrics.add(name="app.trend.rate")
s.sum.aggregation_temporality = m.AGGREGATION_TEMPORALITY_CUMULATIVE
s.sum.is_monotonic = True
s.sum.data_points.add(start_time_unix_nano=1700000100000000000, time_unix_nano=NOW, as_double=2.5)

data = req.SerializeToString()
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "otlp_vectors.h")
with open(out, "w") as f:
    f.write("/* Written by tests/unit/gen-otlp-vectors.py with opentelemetry-proto: do not edit. */\n")
    f.write("static const unsigned char otlp_golden[] = {\n")
    for i in range(0, len(data), 16):
        f.write("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",\n")
    f.write("};\n")
print(f"{out}: {len(data)} bytes")
