# OCUDU Documentation

This directory contains the automated API documentation generation (Doxygen) for OCUDU code,
plus **Git-rendered Markdown** for the optional dApp Runtime and E3 management plane.

## Structure

```txt
docs/
├── .env                     # env file for docker-compose
├── docker-compose.yml       # Docker services for documentation
├── dapp/                    # dApp + E3 guides (Markdown; renders on GitHub/GitLab)
├── doxygen/                 # Doxygen project (HTML; OCUDU architecture style)
└── README.md                # This file
```

## dApp Runtime and E3 (start here for plugins)

See **[dapp/README.md](dapp/README.md)** for the index of quickstart, YAML
configuration, E3 management, wire format, and system design guides. Matching
Doxygen pages (`dapp_*`, `e3_*`) are built with the `doxygen` CMake target.

## Docker Services

### Usage

Builds doxygen locally with

```bash
docker compose -f docs/docker-compose.yml up
```

You can select another doxygen target using the environmental variable

```bash
DOXYGEN_TARGET=doxygen-support docker compose -f docs/docker-compose.yml up
```

### Environment Variables

To run the docker-compose, you may need to adjust the variables defined in the .env file.

- `UID`/`GID`: Your user/group IDs for file permissions in Docker
