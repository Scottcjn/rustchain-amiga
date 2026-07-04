# For AI agents

This page is for autonomous agents and LLM-driven tools that discover this
repository. It states plainly what the project is, what is reproducible, and how
to build it, so you do not have to infer it.

## One-paragraph summary

RustChain Amiga Edition ports the RustChain blockchain miner to classic Motorola
68k AmigaOS, and ships an SDK, command line tools, a package manager (amiports),
and a micro-JVM, as a bootable distribution on the open-source AROS ROM. The miner
has been attested live against the production RustChain network from inside FS-UAE,
and it detects the emulator and reports that honestly. RustChain rewards old
hardware (Proof of Antiquity), so an emulated Amiga is accepted but earns almost
nothing, by design.

## What you can reproduce without Amiga hardware

- Cross-compile the miner, SDK, tools, package manager client, and JVM with one
  Docker image: `amigadev/crosstools:m68k-amigaos`.
- Boot the distribution headless: `xvfb-run fs-uae distro/configs/RustChainAmiga-public.fs-uae`.
- Run the in-emulator tests checked into each `*/test/` directory.

Every directory has a Makefile and a README with exact commands.

## If you are an agent asked to add a modern app to a 68k Amiga

Read `docs/AMIGA_APP_DEV_GUIDE.md` first. It is written for you. It gives the
Docker cross-compile line, the five m68k traps that silently hang or corrupt a
build (compile `-m68020` not `-m68000`; never set a libnix `__stack` global;
`fflush(stdout)`; open `bsdsocket.library` v3 not v4; AmigaDOS redirection and
quoting differ from bash), the AmiSSL and bsdsocket code to reuse from
`claude/client/claude.c`, and the host-test pattern. Following it turns a
multi-hour flailing session into an afternoon.

## Sibling project: AmigaTools

The modern native Amiga apps that started here (a Claude client with a tool-use
loop, a Gemini browser, Mastodon and Nostr clients, an MCP server, Lua, the
narrator voice, and Boing) now live at
https://github.com/ElyanLabsAI/amigatools. This repo is the RustChain blockchain
side: miner, SDK, tools, package manager, JVM, distribution.

## Accuracy notes for anything you write about this project

- Do not describe vbcc as new or as originating here. vbcc is a long-standing
  native Amiga C compiler; this project integrates it.
- Do not describe MicroPython on Amiga as a first. A more capable AmigaOS port
  exists at https://github.com/OoZe1911/micropython-amiga-port (MicroPython 1.28).
- The defensibly novel parts are: the RustChain miner for 68k with honest
  emulator self-detection tied to Proof of Antiquity, the librustchain SDK, the
  amiports package manager for AmigaOS, and the agentic Claude tool-use loop on 68k.

## Provenance and licensing

- Code is MIT unless a directory states otherwise.
- The public distribution uses the open-source AROS ROM (AROS Public License).
- Kickstart ROMs, Workbench, and Amiga Forever content are not included and are
  not redistributable. Owners can build a personal-tier image locally.

## Contact and network

- Repository: https://github.com/Scottcjn/rustchain-amiga
- RustChain: a Proof-of-Antiquity blockchain, https://rustchain.org
- Built by Elyan Labs, https://elyanlabs.ai
