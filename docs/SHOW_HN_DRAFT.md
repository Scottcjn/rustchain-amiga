# Show HN draft (HOLD - do not post until Scott approves)

## Title options (pick one)

1. Show HN: RustChain on a 1992 Amiga, a miner that admits it is emulated
2. Show HN: Porting a blockchain miner to classic 68k AmigaOS (and it self-flags in an emulator)
3. Show HN: A self-hosting dev stack for classic AmigaOS: miner, SDK, package manager, micro-JVM

Recommended: #1. Concrete, honest, and the "admits it is emulated" hook is the novel part.

## Body

I ported the RustChain miner to classic Motorola 68k AmigaOS. RustChain uses
Proof of Antiquity: older hardware earns a higher reward multiplier, and
emulators are supposed to be detected. So the interesting design question was
not how to cheat the reward, it was how to be honest about it.

The miner detects UAE (it checks for uae.resource) and reports that to the
server truthfully. An emulated Amiga is accepted, flagged, and earns almost
nothing. A real Amiga reports clean and earns the real multiplier. It has
attested live against the production network from inside FS-UAE, and shows up
flagged, which is exactly what should happen.

Along the way it turned into a small self-hosting stack, all on the free
open-source AROS ROM so anyone can boot it without a Kickstart:

- A single-file C89 miner (CPU detection via ExecBase AttnFlags, Kickstart ROM
  hashing, hardware fingerprint evidence, bsdsocket networking)
- librustchain, an SDK so other people can write RustChain apps for AmigaOS
- amiports, a MacPorts-style package manager with an on-Amiga client that
  fetches, SHA-1 verifies, and installs packages over HTTP
- mjvm, a tiny JVM that runs javac-produced bytecode on 68k (a documented
  subset: int, String, int arrays, real Java semantics, no class library or GC)

Two things that ate the most time and might interest people here:

1. bebbo's gcc 6.5 (the standard m68k-amigaos cross compiler) miscompiles
   64-bit shifts at -m68000. A variance calculation came out as exactly 2^34.
   The fix was to make anything in the trust path shift-free; the integer
   sqrt is a binary search on purpose. There is a second host test that builds
   the same code in 32-bit ILP32 so this class of bug can not hide until it
   reaches real silicon.

2. The AROS replacement ROM does not ship mathieeedoubbas/doubtrans, and the
   native vbcc assembler needs them. So we wrote those two math libraries from
   scratch in 68k assembly using the FPU. That is the kind of thing you only
   discover by actually booting the thing.

Honest scope, because this crowd will and should check:

- vbcc is a long-standing native Amiga C compiler. We package it, we did not
  write it.
- MicroPython on Amiga has prior art, including a more capable 1.28 AmigaOS
  port at github.com/OoZe1911/micropython-amiga-port. Our Python is an
  integration, not a first.
- The parts that are actually new are the honest-emulation miner, the SDK, the
  package manager, and the from-scratch math libraries.

Repo: https://github.com/Scottcjn/rustchain-amiga
It builds from one Docker image and every test runs headless under FS-UAE, so
you can reproduce all of it without Amiga hardware.

## Pre-post checklist

- [ ] Java either grown past the toy subset OR the wording above is precise (it is)
- [ ] Decide: keep our MicroPython build or adopt/credit the upstream 1.28 port
- [ ] Screenshots in the README render on github
- [ ] No "Make Amiga Great Again" in the HN post (keep it professional)
- [ ] Repo README top matches the honest claims here
- [ ] Post Tue-Thu morning US Eastern, be around to answer comments
