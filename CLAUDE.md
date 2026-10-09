# open-ipc

## The design is binding

[docs/DESIGN.md](docs/DESIGN.md) holds the architecture rules (R1–R9), coding conventions and milestones for the ESP32 work. Follow it as written.

- Never edit docs/DESIGN.md or this file unless the user explicitly asks for that specific change.
- If code can't follow the design, stop and tell the user which rule is in the way and why. Don't work around it, and don't quietly change the design to match the code.
- Answering an open question in DESIGN.md is a design change too: propose it, and edit only once the user agrees.

## Branches

- `main` always has a working PC setup (`tx.sh` → `rx.sh`).
- ESP32 work is done on one branch per milestone in DESIGN.md, named `feat/esp32-m<N>-<topic>` and branched from `main`.
- A milestone branch is merged into `main` only when its "Done when" check passes, and only with the user's OK.
- A change to the PC scripts is a commit of its own, and the existing H.264 path must still work after it.

## Tickets

- Tickets are GitHub Issues under the milestone they belong to (M0–M5), labelled `esp32`, `pc` or `decision`.
- Every commit references its ticket: `Refs #N`, or `Closes #N` in the commit that finishes it.
