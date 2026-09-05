# matopt docs — systems-level optimization of applied math

Start here. Each page is self-contained; read in order for the full course.

| # | Page | What you learn |
|---|---|---|
| 1 | [Math background](01-math-background.md) | FLOPs, memory traffic, arithmetic intensity, fp error |
| 2 | [Systems background](02-systems-background.md) | Cache, TLB, SIMD, threads, syscalls we use |
| 3 | [Ladder walkthrough](03-ladder-walkthrough.md) | optim_1 → optim_12, rung by rung, with code refs |
| 4 | [Peak & roofline](04-peak-and-roofline.md) | Packing theory, roofline method, what blocks the last mile |
| 5 | [Portability & security](05-portability-security.md) | OS matrix, shim design, safety audit |
| 6 | [Exercises](06-exercises.md) | optim_13 and beyond, with acceptance criteria |

Conventions: `file.c:line` refs are approximate (search the symbol if the
line drifted). All timings quoted are from a 2-vCPU EPYC 7763 unless noted.
Build everything with `make && make run` from the repo root.
