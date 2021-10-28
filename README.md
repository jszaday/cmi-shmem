# CMI SHMEM Module

Goal:
- Enable fast IPC communication with POSIX SHM (SHMEM) or XPMEM.

Currently in early stages. Lots of work remains, e.g.:
- Enable expanding SHMEM region when it runs out.
- Enable coalescing and splitting blocks.
- ~~Integrate support for XPMEM.~~
- Enable `CmiFree` to Identify IPC Blocks.

High level overview of API:
https://docs.google.com/document/d/1m0Ohx8gDuq0uoOOwFQHHCy-JL8_xf95vor8yC0gslwk/edit?usp=sharing
