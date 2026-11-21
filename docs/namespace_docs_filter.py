# SPDX-License-Identifier: MIT
"""Doxygen input filter that documents each plugin's unnamed namespace.

Every component library keeps its classes in an unnamed namespace, so they have internal
linkage and the library exports only the two C entry points that
HYDROCOUPLE_DECLARE_COMPONENT emits. Doxygen lists such a namespace as
anonymous_namespace{<file>}, and its \\namespace command cannot name one, so the
descriptions cannot live in a .dox file. This filter puts each description in front of
the file's first "namespace" line instead, on that same line, so warning and definition
line numbers still match the source. FILTER_SOURCE_FILES stays NO: the source listings
show the files unchanged.

Doxygen runs it as ``python3 namespace_docs_filter.py <file>`` (FILTER_PATTERNS in the
Doxyfile) and reads the filtered text from standard output. A source file with no entry
below gets a "warning:" line on standard error, which the documentation workflow counts.
"""
import os
import re
import sys

EXPORTS = ("Internal linkage: the library exports only the two C entry points that "
           "HYDROCOUPLE_DECLARE_COMPONENT emits.")

DESCRIPTIONS = {
    "basicadapterscomponent.cpp":
        "The Basic Adapters plugin (hydrocouple.components.adapters.basic): a standalone "
        "adapted-output factory offering negate and clamp over rank-1 Float64 outputs.",
    "combinercomponent.cpp":
        "The Combiner plugin (hydrocouple.components.combiner): a multi-input that takes "
        "providers under the augend and addend roles, and the component that sums, "
        "multiplies or averages them each step.",
    "gaintransformercomponent.cpp":
        "The Gain Transformer plugin (hydrocouple.components.gain): y = gain * x + bias on "
        "one input, each step.",
    "intervalcomponents.cpp":
        "The interval-transfer reference pair, built twice from this one file: "
        "org.hydrocouple.openswmm.test.interval-source (hcc_intervalsource) and "
        "org.hydrocouple.openswmm.test.sink (hcc_intervalreceiver), with their "
        "configuration argument, typed interval inputs and shared base class.",
    "meshgeneratorcomponent.cpp":
        "The Mesh Generator plugin (hydrocouple.components.meshgenerator): triangulates a "
        "boundary with the SDK Triangulator, optionally merges it to quad-dominant with "
        "the QuadMesher, and serves the mesh as a polyhedral-surface output.",
    "recordercomponent.cpp":
        "The Recorder plugin (hydrocouple.components.recorder): writes one step,value CSV "
        "row per step from its one input.",
    "signalsourcecomponent.cpp":
        "The Signal Source plugin (hydrocouple.components.signal_source): one sample of a "
        "sine, square, ramp or constant waveform per step.",
    "smokecomponent.cpp":
        "The smoke plugin (hydrocouple.components.smoke): the smallest complete component, "
        "one output set to 42 in one step; a test fixture for the loading convention.",
    "timeseriesprovidercomponent.cpp":
        "The Time Series Provider plugin (hydrocouple.components.timeseriesprovider): reads "
        "CSV, CE-QUAL-W2 .npt and SWMM-style .dat files and USGS NWIS JSON into one "
        "time-series output per series; the format readers are free functions.",
}

NAMESPACE = re.compile(r"^namespace\s*(\{\s*)?$")


def main() -> None:
    source = sys.argv[1]
    name = os.path.basename(source)
    text = DESCRIPTIONS.get(name)
    done = False
    with open(source, encoding="utf-8") as stream:
        for number, line in enumerate(stream, start=1):
            if not done and NAMESPACE.match(line):
                done = True
                if text:
                    line = f"/*! {text} {EXPORTS} */ {line}"
                else:
                    sys.stderr.write(f"{source}:{number}: warning: no namespace description "
                                     f"for {name} in docs/namespace_docs_filter.py\n")
            sys.stdout.write(line)


if __name__ == "__main__":
    main()
