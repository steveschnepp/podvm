# ADR-025: Escape the podvm.*= Config Channel

**Date:** 2026-10-08

**Status:** Implemented

## Context

The host passes configuration to the guest as a space-delimited list of
`podvm.key=value` tokens, delivered over two channels that must agree:
the kernel command line (ADR-006) and a vsock line (ADR-016). The guest
splits tokens on spaces (`kv_find`) and, for `podvm.args` and
`podvm.env`, splits each value again on commas (`csv_parse`).

This means a value containing a space is truncated at the token boundary
and a value containing a comma is split into two list elements. Until now
that was tolerable: args came from the CLI and users rarely typed spaces,
and env was a single `HOME=/root`. Two changes made it a real defect.

First, image config support (this session) means the guest's command,
working directory, and environment can come from an image's OCI config
blob. An image whose `Cmd` is `["sh","-c","echo hi"]` -- extremely
common -- could not run with no CLI command, because the `-c` script is
one argument containing spaces. A `WORKDIR "/My App"` broke the same way.

Second, an argument containing a comma is indistinguishable from an
argument separator, so `echo "a,b"` reached the guest as two arguments.

The channel is also the compatibility surface: the kernel cmdline is the
authoritative fallback whenever vsock is unavailable, so any encoding
change must be identical on both, or the fallback would behave
differently from the primary path.

## Decision

Introduce backslash escaping on the `podvm.*=` channel, applied uniformly
to both the kernel cmdline and the vsock payload:

- `\` escapes the next character.
- `\\` is a literal backslash, `\ ` a literal space, `\,` a literal
  comma.
- `kv_find` and `kv_copy_value` treat only an *unescaped* space as a
  token boundary; `csv_parse` treats only an *unescaped* comma as a
  separator and unescapes each field.

The host escapes every value at wire time in a single shared builder
(`build_config_tokens`) used by both channels, escaping `args` and `env`
element-wise so the comma separators survive as separators.

## Rationale

Backslash escaping is the smallest change that makes the existing format
lossless, and it is backwards compatible: a value containing no special
character encodes to itself, so the guest and host can be upgraded
independently and an unescaped cmdline still parses exactly as before.

The escaping lives entirely in `strparse.c`, which both the host (for
building) and the guest (for parsing) already compile, so the encoder and
decoder cannot drift apart. Centralizing the wire encoding in
`build_config_tokens` means the cmdline and vsock paths share one encoder
and cannot diverge on which characters they escape -- the failure mode
that would make the cmdline fallback behave differently from vsock.

Escaping `args`/`env` per element rather than per whole string is
essential: escaping the whole string would also escape the separator
commas and collapse the list into a single field on the guest. The
element-wise loop keeps separators as real commas while making any comma
*inside* an element literal.

## Implications

`kv_copy_value` no longer returns a ready-to-use value for
single-valued fields: it passes escape sequences through verbatim, so the
guest must call `kv_unescape` on `root`, `entrypoint`, `cwd`, and
`hostname` after copying. `csv_parse` unescapes its own fields, so `args`
and `env` need no extra step. Any new single-valued field must remember
to unescape.

The `-` sentinel for empty `args`/`env` is unchanged and is not escaped
(a lone `-` contains no special character), so the empty-value contract
holds.

Kernel cmdline length is the binding constraint. Escaping grows values
(space and comma each cost one extra byte, backslash two), and the
cmdline buffer is 4096 bytes. Long env lists with many spaces will hit
that ceiling sooner than before. This is a pre-existing limit that
escaping makes mildly tighter; a length-prefixed binary vsock framing
would lift it but is not warranted yet (see Alternatives).

## Alternatives Considered

**Base64 or percent-encoding the whole value.** Rejected: it makes the
cmdline unreadable in `/proc/cmdline`, which is the main debugging window
for guest config, and it expands every value (including paths) rather
than only the characters that need it.

**Length-prefixed framing on vsock.** The clean long-term answer, since
it removes all character constraints and lifts the cmdline-size ceiling.
Deferred: it changes the vsock protocol and does not help the cmdline
fallback, which must stay text. Revisit if the 4096-byte cmdline limit
actually binds.

**A structured channel (JSON) for config.** Rejected for now: the guest
has no JSON parser in the initramfs, and adding one to hit a problem that
backslash escaping solves is disproportionate.
