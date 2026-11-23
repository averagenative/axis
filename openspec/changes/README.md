# Changes

New work on Axis is proposed here. Each change is a folder, `openspec/changes/<name>/`, that holds:

- `proposal.md`: why the change is wanted and what it changes.
- `design.md`: the reasoning, when the change needs one.
- `specs/<capability>/spec.md`: the requirements that the change adds, modifies or removes.
- `tasks.md`: the work, with a task for the test of each scenario.

Keep a change to one small slice. When it is built and its gates pass, archive it with `openspec archive <name>`, which merges its requirements into `openspec/specs/`.

Before you write a proposal, read `openspec/project.md`. Use made-up titles, release names, identifiers and hosts in every example.
