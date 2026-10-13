# Contributing

Thanks for helping. Issues and pull requests are welcome here.

## How changes land

This repository is a **published mirror**. Development happens in a private monorepo, and each release is exported here as one commit (the `Source:` trailer names the monorepo commit).

When you open a pull request:

1. The CLA check asks you to sign the [Contributor License Agreement](CLA.md) once. You keep your copyright.
2. A maintainer reviews it here as usual.
3. When it's accepted, the maintainer imports your change into the monorepo with you as the commit author. It ships in the next publish, and we close your PR with a link to that commit as "merged upstream". We never merge directly on this repo, so don't worry when the PR shows as closed rather than merged.

Please keep pull requests to one change each, and don't edit `README.md`, `LICENSE`, `NOTICE`, `SECURITY.md`, `CONTRIBUTING.md`, `DISCLAIMER` or `REUSE.toml`; those files are generated. Open an issue for changes to them instead.

## Safety first

Read [DISCLAIMER](DISCLAIMER). Changes that touch the UV-C lamp, the heater or the interlocks need a clear explanation of how you tested them. UV-C stays off unless a person explicitly turns it on.

## Licenses

This repository is licensed under GPL-3.0-or-later (see [LICENSE](LICENSE)); [REUSE.toml](REUSE.toml) lists the license of every file. Names and logos are covered by [TRADEMARKS.md](TRADEMARKS.md), not by the license.
