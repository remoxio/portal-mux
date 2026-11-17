# moonlight-common-c (vendored)

Moonlight's GameStream/Sunshine protocol library, GPL-3.0 (`LICENSE.txt`), unmodified except that
docs, project files and tests were left out:

- moonlight-common-c `f900dd4767759c7b9d0e93bcea666b55c69ea62f` — the commit moonlight-qt v6.2.0 pins
- `enet/` cgutman/enet `aca87840b57f045a1f7f9299e4b1b9b8e2a5e2f1` (MIT, `enet/LICENSE`)
- `nanors/` sleepybishop/nanors `b1e3c22ca0cdc0bb83e3cd6ed1a2fc77869ed99a` (MIT, `nanors/LICENSE`)

Not packaged by the usual OS package managers, hence vendored. Update together with the pairing and
launch code in `src/moonlight/`, checked against moonlight-qt's `app/backend/nvhttp.cpp` and `nvpairingmanager.cpp`.
