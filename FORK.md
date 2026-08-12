# Suvio Nucleus fork

This repository tracks public Nucleus releases and carries the smallest patch
needed by KMediaPlayer and Suvio for native media composition and HDR/EDR output.
Fork releases keep the upstream version and append `-kmp-hdr.N`, for example
`2.4.0-kmp-hdr.1`.

The fork's `main` branch must always be exactly one commit ahead of the matching
upstream release. Follow-up changes are amended into that single fork commit;
they are not appended as additional commits.

## Maven repository

Published releases are available without credentials from:

```kotlin
maven("https://suviomedia.github.io/Nucleus/maven/")
```

The same repository must be present in `pluginManagement.repositories` when the
forked Nucleus Gradle plugin is used. Coordinates stay compatible with upstream;
consumers select the fork explicitly through its `-kmp-hdr.N` version.

## Publishing

The **Publish Suvio HDR Fork** GitHub Actions workflow accepts a version such as
`2.4.0-kmp-hdr.1`. It performs the complete release remotely:

1. verifies that the fork is exactly one commit ahead of the matching upstream tag;
2. builds native libraries on GitHub-hosted macOS, Windows, and Linux runners;
3. runs the upstream release checks;
4. publishes every runtime module and the Gradle plugin into a Maven repository;
5. creates the fork tag and GitHub Release only after the build succeeds;
6. rebuilds the public Maven repository on GitHub Pages from immutable Releases.

The workflow creates the tag with `GITHUB_TOKEN`. GitHub intentionally does not
fan that event out into the inherited upstream tag workflows, so upstream Maven
Central and Plugin Portal credentials are not required.

## Updating from upstream

For each public Nucleus release:

1. fetch the new upstream tag;
2. rebase the one native-media/HDR commit onto that tag;
3. amend any follow-up work into the same commit with `git commit --amend`;
4. verify that `git rev-list --count vX.Y.Z..main` prints `1`;
5. run Nucleus and KMediaPlayer integration tests;
6. update the fork version trusted by dependency verification;
7. update the fork's `main` with `git push --force-with-lease origin main`;
8. dispatch **Publish Suvio HDR Fork** with `X.Y.Z-kmp-hdr.N`.

The rebase shape is deliberately simple:

```shell
git fetch upstream tag vX.Y.Z
git rebase --onto vX.Y.Z HEAD^ main
```

When upstream has not changed, skip the rebase and amend the current commit
directly. The publishing workflow independently rejects histories that are not
exactly one fork commit above the requested upstream tag.

Do not add unrelated product behavior to this fork. Changes beyond release
automation should remain limited to the native-media bridge, correct native
window composition, and HDR/EDR color delivery required by KMediaPlayer.
