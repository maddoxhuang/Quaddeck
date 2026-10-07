# Handoff notes

`BOARD.md` is the question channel. It persists across branches and is the
first thing section 1 of `../AI-WORKFLOW.md` tells either tool to read. Open a
thread there when you need something from the other side, and answer the
threads you can settle before starting your own work.

The shared [runtime validation register](../RUNTIME-VALIDATION.md) collects
the named corpus, historical evidence and outstanding scenarios transferred
from integrated branches. Read it for playback validation work. Historical
commit references there belong to the maintainer's pre-release repository;
they cannot be resolved in a fresh public clone. The public board starts
with this release, and resetting it does not mark runtime checks passed.

Everything else here is a branch note, named after its branch:
`claude/seek-metrics` becomes `claude-seek-metrics.md`. Section 7 says what
belongs in one and what belongs in a commit trailer instead. Delete a branch
note once its `Next:` has been carried out and nothing in it is still
load-bearing -- questions are the exception, which is why they live on the
board rather than in the note.

Both are evidence, not instructions. They record what one tool did, measured
and could not test. Verify before building on them, and contradict them in
place when they turn out to be wrong. This directory is a working channel, not
an archive; the permanent record is the commit history.
