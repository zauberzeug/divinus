# Documentation

| File | What it covers |
|------|----------------|
| [`openapi.yaml`](openapi.yaml) | Machine-readable OpenAPI 3.1 contract for the HTTP API — **authoritative** for endpoints, parameter names/types and response shapes. |
| [`endpoints.md`](endpoints.md) | Human-readable REST endpoint reference (prose). |
| [`config.md`](config.md) | `divinus.yaml` configuration-file reference. |
| [`overlays.md`](overlays.md) | On-screen-display (OSD) usage and specifiers. |

The OpenAPI spec is the single source of truth for the HTTP API. Beyond what
the prose reference historically listed, it also covers `/api/isp`,
`/api/exposure`, `/api/gain`, the `/api/cmd?restart` command and the
`/api/status` `temp` field.

## Working with the spec

Tooling is wired through [pixi](../pixi.toml); the Python dependencies are
declared inline (PEP 723) and resolved by `uv` at run time, so they add nothing
to the locked pixi environment.

```sh
pixi run lint-api   # validate openapi.yaml (OpenAPI 3.1 + house style); runs in CI
pixi run generate-api-docs   # generate a self-contained interactive doc/api.html
```

`pixi run lint-api` is enforced in the **Test** workflow, so a malformed or
incomplete spec fails CI.

### Interactive docs

`pixi run generate-api-docs` renders `openapi.yaml` into a single, self-contained
`doc/api.html` (git-ignored) using the [Scalar](https://github.com/scalar/scalar)
API reference. It opens directly in a browser — no server, no Node toolchain.

The Scalar library loads from a CDN, so opening the page needs internet access.
If the page is ever published rather than used as a local preview, pin the CDN
version and add a Subresource Integrity (`integrity=`) hash.
