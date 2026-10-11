#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Standalone synthetic GF5298 exposure regressions; no hardware or private data.

Run: python3 tests/goodix521d-exposure.py
CC and CFLAGS may override the standalone C11 compiler/flags. An existing
shared test library may instead be supplied as GOODIX_EXPOSURE_LIBRARY.

Frozen oracle provenance: authored synthetic 80x64 planes, mode29/mode1,
initializer5776c, update57b0c and distribution58888 from the pinned WBDI
SHA256 a6357a78c14fe1b82e4bad495fdf39d9fbe4af93193a14ee23a250e3f544553d.
The expected values below were captured independently before this C module
existed. No vendor code, binary or biometric samples are needed by these tests.
State objects are losslessly stored as arrays using STATE_FIELDS; only timing
metadata is omitted. Never regenerate expected values from this C module.
Source vectors.json SHA256: 43cc03a3e7cfad0cda3052e0b0ade3ca6f1d1e8cc61adc2987e7ee9dfcf691a1
Source shift-vectors.json SHA256: 62a6c158ed441c91170969c2d0c69dc6a72dc65966e2a737484f04822a512072
Additional minimum-anchor-check.json SHA256: 401146b2321bc485b992c798e5638453e62a2efec77ddb021d8630a7eb1ae0e3
"""

import ctypes as C
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

STATE_FIELDS = ['slope', 'dac', 'lower', 'upper', 'outer_band', 'inner_band', 'mean', 'adjusting', 'steps', 'high_count', 'low_count']
VECTORS = json.loads(r'''[
{"name":"bright_history","integration":100,"initial":161,"initial_state":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"flat","value":3500},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],3500,0,0,1,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":3500},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],3500,0,0,1,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],3500,0,0,2,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":3500},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],3500,0,0,2,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,170,151,186,[800,3000],[1100,2700],3500,1,1,3,0],"dacs_after":[170,173,179,184],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":3500},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,170,151,186,[800,3000],[1100,2700],3500,1,1,3,0],"dacs_before":[170,173,179,184],"after":[32.599998474121094,179,151,186,[800,3000],[1100,2700],3500,1,2,3,0],"dacs_after":[179,182,188,193],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":3500},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,179,151,186,[800,3000],[1100,2700],3500,1,2,3,0],"dacs_before":[179,182,188,193],"after":[32.599998474121094,186,151,186,[800,3000],[1100,2700],3500,1,3,3,0],"dacs_after":[186,189,195,200],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}}]},
{"name":"dark_history","integration":100,"initial":161,"initial_state":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"flat","value":300},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],300,0,0,0,1],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":300},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],300,0,0,0,1],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],300,0,0,0,2],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":300},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],300,0,0,0,2],"dacs_before":[161,164,170,175],"after":[32.599998474121094,152,151,186,[800,3000],[1100,2700],300,1,1,0,3],"dacs_after":[152,155,161,166],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":300},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,152,151,186,[800,3000],[1100,2700],300,1,1,0,3],"dacs_before":[152,155,161,166],"after":[32.599998474121094,151,151,186,[800,3000],[1100,2700],300,1,2,0,3],"dacs_after":[151,154,160,165],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":300},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,151,151,186,[800,3000],[1100,2700],300,1,2,0,3],"dacs_before":[151,154,160,165],"after":[32.599998474121094,151,151,186,[800,3000],[1100,2700],300,1,3,0,3],"dacs_after":[151,154,160,165],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}}]},
{"name":"saturation","integration":100,"initial":161,"initial_state":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"flat","value":4095},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":0,"eligible":false}},{"input":{"kind":"flat","value":4095},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":0,"eligible":false}},{"input":{"kind":"flat","value":4095},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":0,"eligible":false}}]},
{"name":"zeros","integration":100,"initial":161,"initial_state":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"flat","value":0},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,1],"dacs_after":[161,164,170,175],"measurement":{"nonzero":0,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":0},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,1],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,2],"dacs_after":[161,164,170,175],"measurement":{"nonzero":0,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":0},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,2],"dacs_before":[161,164,170,175],"after":[32.599998474121094,152,151,186,[800,3000],[1100,2700],0,1,1,0,3],"dacs_after":[152,155,161,166],"measurement":{"nonzero":0,"selected":4836,"eligible":true}}]},
{"name":"valid_saturated_valid","integration":100,"initial":161,"initial_state":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"flat","value":2000},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],2000,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":4095},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],2000,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":0,"eligible":false}},{"input":{"kind":"flat","value":4095},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":0,"eligible":false}},{"input":{"kind":"flat","value":2000},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],2000,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}}]},
{"name":"hysteresis","integration":100,"initial":161,"initial_state":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"flat","value":3500},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],3500,0,0,1,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":3500},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],3500,0,0,1,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],3500,0,0,2,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":3500},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],3500,0,0,2,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,170,151,186,[800,3000],[1100,2700],3500,1,1,3,0],"dacs_after":[170,173,179,184],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":2900},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,170,151,186,[800,3000],[1100,2700],3500,1,1,3,0],"dacs_before":[170,173,179,184],"after":[32.599998474121094,176,151,186,[800,3000],[1100,2700],2900,1,2,3,0],"dacs_after":[176,179,185,190],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":2700},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,176,151,186,[800,3000],[1100,2700],2900,1,2,3,0],"dacs_before":[176,179,185,190],"after":[32.599998474121094,176,151,186,[800,3000],[1100,2700],2700,0,2,0,0],"dacs_after":[176,179,185,190],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":799},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,176,151,186,[800,3000],[1100,2700],2700,0,2,0,0],"dacs_before":[176,179,185,190],"after":[32.599998474121094,176,151,186,[800,3000],[1100,2700],799,0,2,0,1],"dacs_after":[176,179,185,190],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":799},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,176,151,186,[800,3000],[1100,2700],799,0,2,0,1],"dacs_before":[176,179,185,190],"after":[32.599998474121094,176,151,186,[800,3000],[1100,2700],799,0,2,0,2],"dacs_after":[176,179,185,190],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":799},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,176,151,186,[800,3000],[1100,2700],799,0,2,0,2],"dacs_before":[176,179,185,190],"after":[32.599998474121094,167,151,186,[800,3000],[1100,2700],799,1,1,0,3],"dacs_after":[167,170,176,181],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":900},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,167,151,186,[800,3000],[1100,2700],799,1,1,0,3],"dacs_before":[167,170,176,181],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],900,1,2,0,3],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":1100},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],900,1,2,0,3],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],1100,0,2,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":2000},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],1100,0,2,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],2000,0,2,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}}]},
{"name":"reinit_retains_history","integration":100,"initial":161,"initial_state":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"flat","value":3500},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],3500,0,0,1,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":3500},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],3500,0,0,1,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],3500,0,0,2,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true},"after_reinit":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,2,0]},{"input":{"kind":"flat","value":3500},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,2,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,170,151,186,[800,3000],[1100,2700],3500,1,1,3,0],"dacs_after":[170,173,179,184],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":3500},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,170,151,186,[800,3000],[1100,2700],3500,1,1,3,0],"dacs_before":[170,173,179,184],"after":[32.599998474121094,179,151,186,[800,3000],[1100,2700],3500,1,2,3,0],"dacs_after":[179,182,188,193],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}}]},
{"name":"orientation","integration":100,"initial":161,"initial_state":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"zero_perimeter"},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],1800,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"ramp"},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],1800,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],1123,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}}]},
{"name":"boundary_799","integration":100,"initial":161,"initial_state":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"flat","value":799},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],799,0,0,0,1],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":799},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],799,0,0,0,1],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],799,0,0,0,2],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":799},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],799,0,0,0,2],"dacs_before":[161,164,170,175],"after":[32.599998474121094,152,151,186,[800,3000],[1100,2700],799,1,1,0,3],"dacs_after":[152,155,161,166],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}}]},
{"name":"boundary_800","integration":100,"initial":161,"initial_state":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"flat","value":800},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],800,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":800},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],800,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],800,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":800},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],800,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],800,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}}]},
{"name":"boundary_1100","integration":100,"initial":161,"initial_state":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"flat","value":1100},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],1100,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":1100},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],1100,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],1100,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":1100},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],1100,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],1100,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}}]},
{"name":"boundary_2700","integration":100,"initial":161,"initial_state":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"flat","value":2700},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],2700,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":2700},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],2700,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],2700,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":2700},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],2700,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],2700,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}}]},
{"name":"boundary_3000","integration":100,"initial":161,"initial_state":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"flat","value":3000},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],3000,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":3000},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],3000,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],3000,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":3000},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],3000,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],3000,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}}]},
{"name":"boundary_3001","integration":100,"initial":161,"initial_state":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"flat","value":3001},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],3001,0,0,1,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":3001},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],3001,0,0,1,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],3001,0,0,2,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":3001},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],3001,0,0,2,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,170,151,186,[800,3000],[1100,2700],3001,1,1,3,0],"dacs_after":[170,173,179,184],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}}]},
{"name":"selection_1934","integration":100,"initial":161,"initial_state":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"interior_prefix","count":1934,"value":1800,"other":4095},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],1800,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":1934,"eligible":false}}]},
{"name":"selection_1935","integration":100,"initial":161,"initial_state":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"interior_prefix","count":1935,"value":1800,"other":4095},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],1800,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":1935,"eligible":true}}]},
{"name":"selection_4594","integration":100,"initial":161,"initial_state":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"interior_prefix","count":4594,"value":1800,"other":4095},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],1800,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4594,"eligible":true}}]},
{"name":"selection_4595","integration":100,"initial":161,"initial_state":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"interior_prefix","count":4595,"value":1800,"other":4095},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],1800,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4595,"eligible":true}}]},
{"name":"nonzero_4594","integration":100,"initial":161,"initial_state":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"interior_prefix","count":4594,"value":4095,"other":0},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,1],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4594,"selected":242,"eligible":true}}]},
{"name":"nonzero_4595","integration":100,"initial":161,"initial_state":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"interior_prefix","count":4595,"value":4095,"other":0},"background":{"kind":"flat","value":1000},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4595,"selected":241,"eligible":false}}]},
{"name":"init_1_0","integration":1,"initial":0,"initial_state":[0.32600000500679016,0,0,511,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"flat","value":3500},"background":{"kind":"flat","value":1000},"before":[0.32600000500679016,0,0,511,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[0,0,0,0],"after":[0.32600000500679016,0,0,511,[800,3000],[1100,2700],3500,0,0,1,0],"dacs_after":[0,0,0,0],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":3500},"background":{"kind":"flat","value":1000},"before":[0.32600000500679016,0,0,511,[800,3000],[1100,2700],3500,0,0,1,0],"dacs_before":[0,0,0,0],"after":[0.32600000500679016,0,0,511,[800,3000],[1100,2700],3500,0,0,2,0],"dacs_after":[0,0,0,0],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":3500},"background":{"kind":"flat","value":1000},"before":[0.32600000500679016,0,0,511,[800,3000],[1100,2700],3500,0,0,2,0],"dacs_before":[0,0,0,0],"after":[0.32600000500679016,511,0,511,[800,3000],[1100,2700],3500,1,1,3,0],"dacs_after":[511,511,511,511],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}}]},
{"name":"init_2_511","integration":2,"initial":511,"initial_state":[0.6520000100135803,511,50,511,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"flat","value":3500},"background":{"kind":"flat","value":1000},"before":[0.6520000100135803,511,50,511,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[511,511,511,511],"after":[0.6520000100135803,511,50,511,[800,3000],[1100,2700],3500,0,0,1,0],"dacs_after":[511,511,511,511],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":3500},"background":{"kind":"flat","value":1000},"before":[0.6520000100135803,511,50,511,[800,3000],[1100,2700],3500,0,0,1,0],"dacs_before":[511,511,511,511],"after":[0.6520000100135803,511,50,511,[800,3000],[1100,2700],3500,0,0,2,0],"dacs_after":[511,511,511,511],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":3500},"background":{"kind":"flat","value":1000},"before":[0.6520000100135803,511,50,511,[800,3000],[1100,2700],3500,0,0,2,0],"dacs_before":[511,511,511,511],"after":[0.6520000100135803,511,50,511,[800,3000],[1100,2700],3500,1,1,3,0],"dacs_after":[511,511,511,511],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}}]},
{"name":"init_510_161","integration":510,"initial":161,"initial_state":[166.25999450683594,161,159,166,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"flat","value":3500},"background":{"kind":"flat","value":1000},"before":[166.25999450683594,161,159,166,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,161,161,161],"after":[166.25999450683594,161,159,166,[800,3000],[1100,2700],3500,0,0,1,0],"dacs_after":[161,161,161,161],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":3500},"background":{"kind":"flat","value":1000},"before":[166.25999450683594,161,159,166,[800,3000],[1100,2700],3500,0,0,1,0],"dacs_before":[161,161,161,161],"after":[166.25999450683594,161,159,166,[800,3000],[1100,2700],3500,0,0,2,0],"dacs_after":[161,161,161,161],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}},{"input":{"kind":"flat","value":3500},"background":{"kind":"flat","value":1000},"before":[166.25999450683594,161,159,166,[800,3000],[1100,2700],3500,0,0,2,0],"dacs_before":[161,161,161,161],"after":[166.25999450683594,162,159,166,[800,3000],[1100,2700],3500,1,1,3,0],"dacs_after":[162,162,162,162],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}}]},
{"name":"cutoff_all_high","integration":100,"initial":161,"initial_state":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"flat","value":100},"background":{"kind":"flat","value":4095},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],100,0,0,0,1],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}}]},
{"name":"cutoff_negative_population","integration":100,"initial":161,"initial_state":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"interior_prefix","count":500,"value":0,"other":4095},"background":{"kind":"flat","value":3900},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,1],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4336,"selected":500,"eligible":true}}]},
{"name":"cutoff_border_only","integration":100,"initial":161,"initial_state":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"zero_perimeter"},"background":{"kind":"flat","value":4095},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],1800,0,0,0,0],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}}]},
{"name":"difference_3800","integration":100,"initial":161,"initial_state":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"flat","value":295},"background":{"kind":"flat","value":4095},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],295,0,0,0,1],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}}]},
{"name":"difference_3801","integration":100,"initial":161,"initial_state":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"steps":[{"input":{"kind":"flat","value":294},"background":{"kind":"flat","value":4095},"before":[32.599998474121094,161,151,186,[800,3000],[1100,2700],0,0,0,0,0],"dacs_before":[161,164,170,175],"after":[32.599998474121094,161,151,186,[800,3000],[1100,2700],294,0,0,0,1],"dacs_after":[161,164,170,175],"measurement":{"nonzero":4836,"selected":4836,"eligible":true}}]}
]''')
SHIFT_VECTORS = json.loads(r'''[{"before":[150,155,160,165],"target":140,"after":[140,145,150,155]},{"before":[500,505,510,511],"target":510,"after":[510,3,8,9]},{"before":[161,150,165,170],"target":160,"after":[171,160,175,180]},{"before":[0,1,510,511],"target":0,"after":[0,1,510,511]}]''')

WIDTH, HEIGHT = 80, 64
PIXELS = WIDTH * HEIGHT
Frame = C.c_uint16 * PIXELS
Dacs = C.c_uint16 * 4
OK, INVALID, STATE_ERROR = 0, 1, 2


class State(C.Structure):
    _fields_ = [(name, C.c_uint16) for name in (
        "integration", "initial_dac", "dac", "lower", "upper", "outer_low",
        "outer_high", "inner_low", "inner_high", "mean", "adjusting", "steps",
        "high_count", "low_count", "initialized")] + [("slope", C.c_float)]


class Measurement(C.Structure):
    _fields_ = [("nonzero", C.c_uint32), ("selected", C.c_uint32),
                ("eligible", C.c_bool)]


def plane(spec):
    """Same authored synthetic generators used to capture the frozen oracle."""
    kind = spec["kind"]
    if kind == "flat":
        values = [spec["value"]] * PIXELS
    elif kind == "ramp":
        values = [500 + 7*x + 11*y for y in range(HEIGHT) for x in range(WIDTH)]
    elif kind == "zero_perimeter":
        values = [0 if x in (0, 79) or y in (0, 63) else 1800
                  for y in range(HEIGHT) for x in range(WIDTH)]
    elif kind == "interior_prefix":
        values = [spec["other"]] * PIXELS
        for i, (y, x) in enumerate((y, x) for y in range(1, 63)
                                  for x in range(1, 79)):
            if i < spec["count"]:
                values[y*WIDTH + x] = spec["value"]
    else:
        raise ValueError(kind)
    return Frame(*values)


def flat(value):
    return plane({"kind": "flat", "value": value})


def snapshot(state):
    values = {name: getattr(state, name) for name in STATE_FIELDS
              if name not in ("outer_band", "inner_band")}
    values["outer_band"] = [state.outer_low, state.outer_high]
    values["inner_band"] = [state.inner_low, state.inner_high]
    return [values[name] for name in STATE_FIELDS]


class Exposure(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        library = os.environ.get("GOODIX_EXPOSURE_LIBRARY")
        if not library:
            cls.temporary = tempfile.TemporaryDirectory(prefix="goodix-exposure-")
            cls.addClassCleanup(cls.temporary.cleanup)
            library = str(Path(cls.temporary.name) / "exposure.so")
            source = (Path(__file__).resolve().parents[1] / "libfprint" /
                      "drivers" / "goodix521d" / "goodix-exposure.c")
            command = (shlex.split(os.environ.get("CC", "cc")) +
                       ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                        "-fPIC", "-shared", "-O2"] +
                       shlex.split(os.environ.get("CFLAGS", "")) +
                       [str(source), "-o", library])
            subprocess.run(command, check=True)
        cls.lib = C.CDLL(library)
        cls.lib.gx_exposure_reset.argtypes = [C.POINTER(State), C.c_uint,
                                               C.c_uint]
        cls.lib.gx_exposure_reset.restype = C.c_int
        cls.lib.gx_exposure_reinitialize.argtypes = [C.POINTER(State),
                                                      C.c_uint, C.c_uint]
        cls.lib.gx_exposure_reinitialize.restype = C.c_int
        cls.lib.gx_exposure_clear.argtypes = [C.POINTER(State)]
        cls.lib.gx_exposure_clear.restype = None
        cls.lib.gx_exposure_shift.argtypes = [C.POINTER(C.c_uint16), C.c_uint]
        cls.lib.gx_exposure_shift.restype = C.c_int
        cls.lib.gx_exposure_update.argtypes = [
            C.POINTER(State), C.POINTER(C.c_uint16), C.c_size_t,
            C.POINTER(C.c_uint16), C.c_size_t, C.POINTER(C.c_uint16),
            C.POINTER(Measurement)]
        cls.lib.gx_exposure_update.restype = C.c_int

    def make_state(self, integration=100, initial=161):
        state = State()
        self.assertEqual(self.lib.gx_exposure_reset(C.byref(state), integration,
                                                   initial), OK)
        self.assertEqual(state.initialized, 1)
        return state

    def update(self, state, frame, dacs=None, background=None, measurement=None):
        if dacs is None:
            dacs = Dacs(161, 164, 170, 175)
        if background is None:
            background = flat(1000)
        if measurement is None:
            measurement = Measurement()
        status = self.lib.gx_exposure_update(
            C.byref(state), background, PIXELS, frame, PIXELS, dacs,
            C.byref(measurement))
        return status, dacs, measurement

    def test_frozen_oracle_all_stateful_updates(self):
        self.assertEqual(len(VECTORS), 28)
        self.assertEqual(sum(len(v["steps"]) for v in VECTORS), 75)
        for scenario in VECTORS:
            with self.subTest(scenario=scenario["name"]):
                state = self.make_state(scenario["integration"], scenario["initial"])
                self.assertEqual(snapshot(state), scenario["initial_state"])
                dacs = Dacs(*scenario["steps"][0]["dacs_before"])
                for index, step in enumerate(scenario["steps"]):
                    with self.subTest(step=index):
                        self.assertEqual(snapshot(state), step["before"])
                        self.assertEqual(list(dacs), step["dacs_before"])
                        background, frame = plane(step["background"]), plane(step["input"])
                        old_background, old_frame = bytes(background), bytes(frame)
                        status, dacs, measurement = self.update(state, frame, dacs, background)
                        self.assertEqual(status, OK)
                        self.assertEqual(snapshot(state), step["after"])
                        self.assertEqual(list(dacs), step["dacs_after"])
                        self.assertEqual({"nonzero": measurement.nonzero,
                                          "selected": measurement.selected,
                                          "eligible": measurement.eligible}, step["measurement"])
                        self.assertEqual(bytes(background), old_background)
                        self.assertEqual(bytes(frame), old_frame)
                        if "after_reinit" in step:
                            self.assertEqual(self.lib.gx_exposure_reinitialize(
                                C.byref(state), scenario["integration"],
                                scenario["initial"]), OK)
                            self.assertEqual(snapshot(state), step["after_reinit"])

    def test_frozen_four_word_distribution(self):
        self.assertEqual(len(SHIFT_VECTORS), 4)
        for case in SHIFT_VECTORS:
            with self.subTest(case=case):
                dacs = Dacs(*case["before"])
                self.assertEqual(self.lib.gx_exposure_shift(dacs, case["target"]), OK)
                self.assertEqual(list(dacs), case["after"])

    def test_reset_starts_new_epoch_and_reinitialize_preserves_history(self):
        state = self.make_state()
        for _ in range(3):
            self.assertEqual(self.update(state, flat(3500))[0], OK)
        self.assertEqual((state.dac, state.adjusting, state.steps, state.high_count),
                         (170, 1, 1, 3))
        self.assertEqual(self.lib.gx_exposure_reinitialize(C.byref(state), 510, 200), OK)
        self.assertEqual((state.dac, state.mean, state.adjusting, state.steps,
                          state.high_count, state.low_count), (200, 0, 1, 1, 3, 0))
        self.assertEqual(self.lib.gx_exposure_reset(C.byref(state), 100, 161), OK)
        self.assertEqual(bytes(state), bytes(self.make_state()))
        self.lib.gx_exposure_clear(C.byref(state))
        self.assertEqual(bytes(state), bytes(State()))
        self.assertEqual(self.update(state, flat(1800))[0], STATE_ERROR)
        self.assertEqual(self.lib.gx_exposure_reinitialize(C.byref(state), 100, 161), STATE_ERROR)
        self.lib.gx_exposure_clear(None)
        self.assertEqual(self.lib.gx_exposure_reset(C.byref(state), 100, 161), OK)

    def test_initializer_rejects_unsafe_inputs_without_mutation(self):
        for function in (self.lib.gx_exposure_reset, self.lib.gx_exposure_reinitialize):
            for integration, initial in ((0, 161), (511, 161), (65536, 161),
                                         (100, 512), (100, 65536), (2**32-1, 0)):
                with self.subTest(function=function.__name__, integration=integration,
                                  initial=initial):
                    state = self.make_state()
                    before = bytes(state)
                    self.assertEqual(function(C.byref(state), integration, initial), INVALID)
                    self.assertEqual(bytes(state), before)
            self.assertEqual(function(None, 100, 161), INVALID)

    def test_update_rejects_unsafe_inputs_atomically(self):
        for invalid in ("background_sample", "frame_sample", "background_short",
                        "frame_short", "background_long", "frame_long", "dac",
                        "null_background", "null_frame", "null_dacs",
                        "null_measurement", "null_state"):
            with self.subTest(invalid=invalid):
                state = self.make_state()
                background, frame = flat(1000), flat(3500)
                dacs, measurement = Dacs(161, 164, 170, 175), Measurement()
                C.memset(C.byref(measurement), 0xA5, C.sizeof(measurement))
                if invalid == "background_sample":
                    background[-1] = 4096
                if invalid == "frame_sample":
                    frame[-1] = 65535
                if invalid == "dac":
                    dacs[3] = 512
                arguments = [C.byref(state), background, PIXELS, frame, PIXELS,
                             dacs, C.byref(measurement)]
                for name, position in (("state", 0), ("background", 1),
                                       ("frame", 3), ("dacs", 5), ("measurement", 6)):
                    if invalid == "null_" + name:
                        arguments[position] = None
                for name, position in (("background", 2), ("frame", 4)):
                    if invalid == name + "_short":
                        arguments[position] = PIXELS-1
                    if invalid == name + "_long":
                        arguments[position] = PIXELS+1
                before = tuple(map(bytes, (state, background, frame, dacs, measurement)))
                self.assertEqual(self.lib.gx_exposure_update(*arguments), INVALID)
                self.assertEqual(tuple(map(bytes, (state, background, frame, dacs,
                                                  measurement))), before)

    def test_overlapping_mutable_buffers_are_rejected_atomically(self):
        for alias in ("dacs_frame", "dacs_background", "dacs_state",
                      "measurement_frame", "measurement_background",
                      "measurement_state", "measurement_dacs"):
            with self.subTest(alias=alias):
                state = self.make_state()
                background, frame = flat(1000), flat(3500)
                dacs = Dacs(161, 164, 170, 175)
                measurement = Measurement(12, 34, True)
                arguments = [C.byref(state), background, PIXELS, frame,
                             PIXELS, dacs, C.byref(measurement)]
                if alias in ("dacs_frame", "dacs_background"):
                    target = frame if alias == "dacs_frame" else background
                    target[1:5] = dacs[:]
                    arguments[5] = C.cast(C.byref(target, 2), C.POINTER(C.c_uint16))
                elif alias == "dacs_state":
                    arguments[5] = C.cast(C.byref(state), C.POINTER(C.c_uint16))
                elif alias == "measurement_dacs":
                    # The DAC output starts inside the measurement output.
                    arguments[5] = C.cast(C.byref(measurement, 2), C.POINTER(C.c_uint16))
                    for i, value in enumerate(dacs):
                        arguments[5][i] = value
                else:
                    target = {"measurement_frame": frame,
                              "measurement_background": background,
                              "measurement_state": state}[alias]
                    arguments[6] = C.cast(C.byref(target, 4), C.POINTER(Measurement))
                before = tuple(map(bytes, (state, background, frame, dacs, measurement)))
                self.assertEqual(self.lib.gx_exposure_update(*arguments), INVALID)
                self.assertEqual(tuple(map(bytes, (state, background, frame, dacs,
                                                  measurement))), before)

    def test_unaligned_minimum_is_distributed_even_for_ineligible_frame(self):
        for value in (3500, 4095):
            state = self.make_state()
            dacs = Dacs(161, 150, 165, 170)
            before = snapshot(state)
            status, dacs, measurement = self.update(state, flat(value), dacs)
            self.assertEqual(status, OK)
            self.assertEqual(state.dac, 161)
            self.assertEqual(list(dacs), [172, 161, 176, 181])
            self.assertEqual(measurement.eligible, value == 3500)
            if value == 4095:
                # Additional pinned wrapper replay, minimum-anchor-check.json.
                self.assertEqual(snapshot(state), before)
                self.assertEqual((measurement.nonzero, measurement.selected), (4836, 0))

    def test_invalid_state_is_rejected_atomically(self):
        corruptions = {"initialized": 0, "integration": 0, "initial_dac": 512,
                       "dac": 512, "lower": 512, "upper": 512, "outer_low": 0,
                       "outer_high": 0, "inner_low": 0, "inner_high": 0,
                       "mean": 4096, "adjusting": 2, "high_count": 4,
                       "low_count": 4, "slope": float("nan")}
        for field, value in corruptions.items():
            with self.subTest(field=field):
                state = self.make_state()
                setattr(state, field, value)
                dacs, measurement = Dacs(161, 164, 170, 175), Measurement(12, 34, True)
                before = tuple(map(bytes, (state, dacs, measurement)))
                self.assertEqual(self.update(state, flat(3500), dacs,
                                             measurement=measurement)[0], STATE_ERROR)
                self.assertEqual(tuple(map(bytes, (state, dacs, measurement))), before)

    def test_shift_rejects_invalid_words_and_target_atomically(self):
        for words, target in (([0, 1, 2, 512], 10), ([0, 1, 2, 65535], 10),
                              ([0, 1, 2, 3], 512), ([0, 1, 2, 3], 65536)):
            dacs = Dacs(*words)
            before = bytes(dacs)
            self.assertEqual(self.lib.gx_exposure_shift(dacs, target), INVALID)
            self.assertEqual(bytes(dacs), before)
        self.assertEqual(self.lib.gx_exposure_shift(None, 161), INVALID)

    def test_step_counter_wrap_is_unsigned_16bit(self):
        state = self.make_state()
        for _ in range(3):
            self.assertEqual(self.update(state, flat(3500))[0], OK)
        state.steps = 65535
        self.assertEqual(self.update(state, flat(3500))[0], OK)
        self.assertEqual(state.steps, 0)
        self.assertEqual(state.dac, 179)

    def test_hysteresis_direction_switch_waits_three_frames(self):
        state = self.make_state()
        dacs = Dacs(161, 164, 170, 175)
        for value, expected in ((3500, (161, 0, 0, 1, 0)),
                                (3500, (161, 0, 0, 2, 0)),
                                (3500, (170, 1, 1, 3, 0)),
                                (300, (170, 0, 1, 3, 1)),
                                (300, (170, 0, 1, 3, 2)),
                                (300, (161, 1, 1, 0, 3))):
            self.assertEqual(self.update(state, flat(value), dacs)[0], OK)
            self.assertEqual((state.dac, state.adjusting, state.steps,
                              state.high_count, state.low_count), expected)

    def test_same_read_only_frame_and_reference_buffer_is_allowed(self):
        state, shared = self.make_state(), flat(1800)
        self.assertEqual(self.update(state, shared, background=shared)[0], OK)
        self.assertEqual(state.mean, 1800)


if __name__ == "__main__":
    unittest.main(verbosity=2)
