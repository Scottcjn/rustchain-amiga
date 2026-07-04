# Amiga MCP Server

A native MCP (Model Context Protocol) server that runs on classic AmigaOS
(m68k). It lets a modern LLM agent drive the Amiga: list tools and call them to
read files, write files, and run AmigaDOS commands on the machine itself.

This is the inverse of `claude/client/claude.c`. That program is an MCP-style
client that runs on the Amiga and calls out to the Anthropic API so Claude can
use tools. This program is the server side of the same tools: the agent runs on
a modern desktop, and this binary runs on the Amiga and does the work. The two
share the same JSON escape and parse helpers and the same three tool handlers,
so they speak the same tool semantics from opposite ends of the wire.

## What it exposes

Three tools, each with a JSON Schema for its input:

| Tool          | Input             | What it does                                  |
|---------------|-------------------|-----------------------------------------------|
| `read_file`   | `path`            | Reads a text file via dos.library Open/Read.  |
| `write_file`  | `path`, `text`    | Writes (overwrites) a file via Open/Write.    |
| `run_command` | `cmd`             | Runs an AmigaDOS command, capturing output.   |

`run_command` runs the command with `Execute(cmd, 0, outfh)` where `outfh` is a
temporary file in `T:`. When the command finishes the server reads that file
back and returns its contents. This is the same capture trick used by
`tool_run_command` in `claude.c`.

## The transport: MCP over stdio

MCP has a standard stdio transport. Messages are JSON-RPC 2.0 objects, one per
line, exchanged over the process's stdin and stdout:

- The client writes a request line to the server's stdin.
- The server reads one line, parses the JSON-RPC request, dispatches it, writes
  exactly one JSON-RPC response line to stdout, and flushes.
- Notifications (messages with no `id`, such as `notifications/initialized`) get
  no response.

stdout is the wire. Nothing but JSON-RPC responses is ever written there. All
logging goes to stderr, so a client that captures stdout only ever sees clean
protocol. The server flushes stdout after every single response, which matters
on the Amiga where a redirected stdout is fully buffered and a later stall must
never swallow a reply that was already produced.

### Methods

| Method                      | Response                                             |
|-----------------------------|------------------------------------------------------|
| `initialize`                | `protocolVersion`, `capabilities {tools:{}}`, `serverInfo` |
| `notifications/initialized` | none (it is a notification)                          |
| `ping`                      | `{}`                                                 |
| `tools/list`                | the three tools with their input schemas             |
| `tools/call`                | dispatches the named tool, returns content blocks    |
| anything else (with an id)  | JSON-RPC error `-32601` method not found             |

A `tools/call` result is an MCP content block list:

```json
{"content":[{"type":"text","text":"...output..."}],"isError":false}
```

Tool failures (a missing file, a bad argument, an unknown tool) come back as a
normal result with `"isError":true` and the reason in the text block. That is
the MCP convention: a tool that ran but failed is not a JSON-RPC protocol error.

### A session looks like this

Request lines in, response lines out. The blank line before the notification
carries no reply:

```
--> {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"agent","version":"1"}}}
<-- {"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2024-11-05","capabilities":{"tools":{}},"serverInfo":{"name":"amiga-mcp","version":"1.0.0"}}}
--> {"jsonrpc":"2.0","method":"notifications/initialized"}
--> {"jsonrpc":"2.0","id":2,"method":"tools/list"}
<-- {"jsonrpc":"2.0","id":2,"result":{"tools":[ ... ]}}
--> {"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"run_command","arguments":{"cmd":"version"}}}
<-- {"jsonrpc":"2.0","id":3,"result":{"content":[{"type":"text","text":"..."}],"isError":false}}
```

## How a client connects to a real Amiga

A desktop MCP client normally spawns a server as a child process and talks to it
over that child's stdin and stdout. On a real Amiga the practical version of
"stdin and stdout" is a serial line or a pipe bridge. You run `mcp-amiga` on the
Amiga with its input and output pointed at the serial port, and on the modern
host you run a tiny bridge that presents that serial port to the MCP client as
if it were a spawned process. The bridge does nothing but move bytes: host stdin
to serial out, serial in to host stdout. Because the protocol is line-delimited
JSON over plain streams, no framing library is needed on either side.

On the Amiga you can point the server at the serial device with the usual shell
redirection, for example running it from a script that has opened `SER:`, or by
launching it with stdin and stdout attached to the serial handler. Any transport
that gives the process a byte-clean stdin and stdout will work: serial, a
null-modem cable, a TCP-to-serial adapter on the host side, or a local pipe when
testing under an emulator.

No network stack, no TLS, and no libraries beyond dos.library are required. That
is deliberate. The client half (`claude.c`) needs AmiSSL and bsdsocket to reach
the internet. This server half needs nothing but the Amiga's own filesystem and
shell, so it runs on a much smaller machine.

## Building

The Amiga binary cross-compiles with the amigadev/crosstools m68k image. The
host self-test builds with the native compiler.

```
make host-test   # native self-test of the protocol and tool logic
make mcp         # Amiga hunk executable at bin/mcp-amiga
make all         # both
make clean
```

`make mcp` runs:

```
docker run --rm -v <repo>:/work -w /work/mcp amigadev/crosstools:m68k-amigaos \
  m68k-amigaos-gcc -noixemul -m68020 -O2 -fomit-frame-pointer \
  -Wall -Wextra -Wno-unused-parameter -o bin/mcp-amiga mcp.c
```

and then prints `file bin/mcp-amiga`, which reports
`AmigaOS loadseg()ble executable/binary`.

### Build notes (each of these cost hours once)

- `-m68020`, not `-m68000`. The bebbo toolchain miscompiles its `__mulsi3` and
  `atoi` helpers at `-m68000` and the binary hangs. Every port in this repo is
  on `-m68020` for that reason. This server never touches that path, but it
  holds the same setting so the whole repo stays on one proven codegen.
- Big buffers are `static` (BSS), never on the stack. AmigaOS task stacks are
  small. There is no libnix `unsigned long __stack` global here.
- stdout is flushed after every response. Debug and status text goes to stderr
  only, so stdout stays pure JSON-RPC.

## Testing

`make host-test` builds `host_test` and runs it. It feeds canned JSON-RPC
requests through the exact same `handle_request` dispatch the Amiga server uses,
and asserts the responses are well formed: the initialize handshake, a
`tools/list` with all three schemas, `tools/call` for write then read then
run_command against `/tmp` on the host (where commands really execute), missing
argument and unknown tool errors coming back as `isError` content, an unknown
method coming back as JSON-RPC `-32601`, and a string id being echoed verbatim.

The same host binary can also run the real stdio wire loop, which is the honest
end to end test of the line framing:

```
printf '%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
  '{"jsonrpc":"2.0","method":"notifications/initialized"}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
  '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"run_command","arguments":{"cmd":"uname -s"}}}' \
  | ./host_test --serve
```

You get one response line per request, the notification produces no line, and
`run_command` returns the real output of the command.

## Files

| File            | Purpose                                                    |
|-----------------|------------------------------------------------------------|
| `mcp.c`         | the whole server: JSON helpers, tools, dispatch, wire loop |
| `Makefile`      | docker cross build and native host self-test               |
| `bin/mcp-amiga` | the cross-compiled AmigaOS m68k executable                 |
