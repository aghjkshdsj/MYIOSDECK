# MYIOSDECK clean room, track B

This folder is the second, independent clean-side implementation of the MYIOSDECK rewrite.
Its writer works from the shared specifications in `..\MYIOSDECK-cleanroom\specs\` and
`..\MYIOSDECK-cleanroom\interfaces\` (read only), under the rules in
`..\MYIOSDECK-cleanroom\CLAUDE.md`, and never reads track A's code in
`..\MYIOSDECK-cleanroom\out\` or its `QUESTIONS.md`.

Layout: one folder per spec (`01-input/`, `02-wine-bridge/`, ...), each with its code,
`TESTS.md`, `PROVENANCE.md` and `CHECKLIST.md`. Questions for the spec writer go in
`QUESTIONS.md` here.
