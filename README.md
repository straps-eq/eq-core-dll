# eq-core-dll

`dinput8.dll` proxies for EverQuest clients that add opt-in client features for EQEmu servers.

| Folder | Client | Notes |
|---|---|---|
| [`tob/`](tob/) | The Outer Brood (x64, build `Sep 11 2025 11:54:10`) | Features are switched on in `dinput8.ini`, no rebuild needed. See [tob/README.md](tob/README.md). |
| [`rof2/`](rof2/) | Rain of Fear 2 (x86, build `May 10 2013`) | The original eq-core-dll; features are chosen at compile time in `src/_options.h`. See [rof2/README.md](rof2/README.md). |

Based on [xackery/eq-core-dll](https://github.com/xackery/eq-core-dll); its history is kept in this repository.
