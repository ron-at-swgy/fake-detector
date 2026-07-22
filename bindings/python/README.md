# fakedetector — Python binding

Pure-Python (`ctypes`, stdlib-only) binding for `libfakedetector`. The
shared library is resolved at import time:

1. `FD_LIBRARY_PATH` environment variable (full path to the `.so`),
2. the system linker path (`make install` puts it there),
3. a copy bundled as package data under `fakedetector/_lib/`.

The import asserts the ABI handshake (`fd_abi_version`) and refuses a
mismatched library.

## Install (e.g. in a policy Docker image)

```dockerfile
# build and install the C library from a pinned release tarball
RUN curl -L https://github.com/<owner>/fake-detector/archive/refs/tags/vX.Y.Z.tar.gz \
      | tar xz && cd fake-detector-* \
    && make shared && make install \
    && pip install bindings/python
```

The rules are embedded in the shared library — no data files to carry.

## Use

```python
from fakedetector import Detector, Phase, VoteAction

with Detector() as fd:              # embedded rules
    fd.register_crewrift()          # ids 0..15 = CrewRift colors
    fd.set_self(0)                  # I am red
    fd.observe_round_config(8, 2)

    fd.observe_self(tick, Phase.PLAYING, "cafeteria")
    fd.observe_player(tick, 2, "cafeteria")     # saw green
    fd.observe_body(tick, 1, "reactor")         # found blue's body

    fd.run()
    d = fd.vote_decision()
    if d.recommendation == VoteAction.CAST:
        chat = fd.render_vote_summary()
```

Run the binding tests with the library built (`make shared` at the repo
root):

```sh
FD_LIBRARY_PATH=$PWD/../../build/libfakedetector.so.1 \
    python3 -m unittest discover -s tests -v
```
