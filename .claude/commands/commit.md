---
description: Write a commit message for the current changes and commit them
argument-hint: [optional hint about the change]
model: haiku
allowed-tools: Bash(git status:*), Bash(git diff:*), Bash(git log:*), Bash(git add:*), Bash(git commit:*)
---
## Context
- Status: !`git status --short`
- Staged summary: !`git diff --staged --stat`
- Staged diff: !`git diff --staged | head -400`
- Recent commits: !`git log --oneline -8`

Hint from user (may be empty): $ARGUMENTS

## Task
1. If nothing is staged, run `git add -u` to stage tracked changes. Don't add untracked files; list any so I can add them myself.
2. Write a commit message that matches the style of the recent commits (prefixes, tense, casing). Default: imperative subject under 72 chars, then a short body only if the why isn't obvious from the subject.
3. Commit it. Reply with just the subject line and the short hash.
