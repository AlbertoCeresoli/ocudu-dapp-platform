# The OCUDU Project

[![Pipeline](https://gitlab.com/ocudu/ocudu/badges/main/pipeline.svg)](https://gitlab.com/ocudu/ocudu/-/pipelines?scope=branches)
[![Documentation](https://img.shields.io/badge/docs-built-green?logo=docusaurus)](https://docs.ocudu.org)
![Code](https://img.shields.io/badge/code-C++17-informational)
![Build](https://img.shields.io/badge/build-CMake-informational)
[![License](https://img.shields.io/badge/license-BSD--3--Clause--Open--MPI-blue)](https://spdx.org/licenses/BSD-3-Clause-Open-MPI.html)
[![OpenSSF Best Practices](https://www.bestpractices.dev/projects/11899/badge)](https://www.bestpractices.dev/projects/11899)
[![Coverage](https://gitlab.com/ocudu/ocudu/badges/main/coverage.svg?min_good=98&min_acceptable=60)](https://docs.ocudu.org/coverage/index.html)

<img src="https://srs.io/wp-content/uploads/ocudu_color.png" alt="image" width="50%"/>

OCUDU is a permissively-licensed, open-source 5G (and beyond) CU/DU project designed for commercial deployment and broad industry adoption, as well as advanced research and development. OCUDU is a complete radio access network (RAN) solution compliant with 3GPP and O-RAN Alliance specifications and includes the full L1/2/3 stack with minimal external dependencies. OCUDU is governed under the Linux Foundation.

This repository contains the RAN source code, architecture documentation, and tooling.

For general information, visit https://ocudu.org.

## Getting started

Build instructions and user guides are provided in the [OCUDU User manual](https://docs.ocudu.org/user_manual/installation/). We also host an extensive selection of tutorials.

## Documentation

Complete project documentation including developer guideline, configuration reference, tutorials, etc. is
hosted in [this](https://gitlab.com/ocudu/ocudu_docs) repo. The most recent version of the documentation
is available [here](https://docs.ocudu.org).

## Optional dApp Runtime and E3

When built with `-DENABLE_DAPP=ON` (and typically `-DENABLE_E3=ON`), the DU can
load hash-bound third-party **dApp** packages (Class A/B/C) from an operator
catalog and manage them over a local **E3** APER endpoint. Optional SCTP
carriers extend the same agent off-process: `dapp.e3ap_sctp` (E3AP-OCUDU-V1,
push telemetry subscriptions plus management as control actions, default port
36423) and `dapp.e3dp_sctp` (FlatBuffers data-profile compatibility endpoint,
default port 38472); both bind loopback by default and have no authentication
in v1.

| Resource | Location |
|----------|----------|
| **Git Markdown index** | [`docs/dapp/README.md`](docs/dapp/README.md) |
| Quickstart | [`docs/dapp/quickstart.md`](docs/dapp/quickstart.md) |
| E3 management / wire / design | [`docs/dapp/e3_management.md`](docs/dapp/e3_management.md), [`e3_wire.md`](docs/dapp/e3_wire.md), [`e3_system_design.md`](docs/dapp/e3_system_design.md) |
| YAML reference | [`docs/dapp/yaml_configuration.md`](docs/dapp/yaml_configuration.md) |
| Doxygen HTML | `cmake --build <build> --target doxygen` (pages `dapp_*`, `e3_*`) |
| Out-of-tree packages | Companion **ocudu-dapp-sdk** repository (`docs/README.md`) |

Without a `dapp:` configuration block the modem path is unchanged even if the
feature is compiled in.

## Contributing

Our project welcomes contributions from any member of our community. To get started contributing,
please take a look at the [Developer Guide](https://docs.ocudu.org/dev_guide/contributing_guide/) with detailed instructions on how to best engange with us.

## Governance

The OCUDU project is governed by a framework of principles, values, policies and processes to help our community and constituents towards our shared goals.

The [Governance](https://gitlab.com/ocudu/Governance) repo is used by the Technical Steering Committee, which oversees governance of the project.

## License

This project is licensed under the BSD 3-Clause Open MPI variant License – see the [LICENSE](./LICENSE) file for details.
Portions of this software may implement 3GPP specifications, which may be subject to additional licensing requirements.
