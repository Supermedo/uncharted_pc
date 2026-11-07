# GitHub desktop builds

The [Build desktop packages workflow](../.github/workflows/release.yml) runs source checks and builds a Linux AppImage and a Windows portable ZIP. Both packages open the same graphical launcher and contain the port, preparation scripts, and required runtime libraries. They do not contain game files. Supply your own decrypted Uncharted collection dump when running a package.

## Manual test build

Open **Actions → Build desktop packages → Run workflow** and choose `main`. The source checks run first, followed by the Linux and Windows jobs. Download `linux-build` and `windows-build` from the workflow run's **Artifacts** section. Manual runs create no GitHub Release.

The equivalent GitHub CLI command is:

```bash
gh workflow run release.yml --ref main -R leomoon/uncharted_pc-work
```

The Linux artifact contains `Uncharted-<revision>-x86_64.AppImage` and its `.sha256` file. The Windows artifact contains `Uncharted-<revision>-windows.zip` and its `.sha256` file. The revision is the nearest reachable `v*` tag plus commit suffix, or a commit hash when no such tag exists. Verify the downloads with `sha256sum --check <asset>.sha256` from the directory containing the files.

## Tagged draft release

After the workflow commit is on the default branch and a manual run has passed, create and push a `v*` tag on the commit to release:

```bash
git tag vX.Y.Z
git push origin vX.Y.Z
```

The tag run builds both packages, checks their SHA-256 files, and creates a **draft** GitHub Release for that exact tag. The release contains the AppImage and ZIP. Checksums remain in the workflow artifacts. Review the release notes, inspect the artifacts, and publish the draft from the Releases page when ready.

The Linux job builds in Debian 13 and uses `packaging/linux/package.sh` to freeze the shared launcher and bundle selected runtime libraries. Double click the AppImage to open the launcher, choose your game folder and game, then select **Play**. The `--play` option starts the last selected game without showing the window. Generated files and saves go to `$XDG_DATA_HOME/uncharted` or `~/.local/share/uncharted`. Both CI builds fetch and package FSR 4 assets and the pinned NVIDIA DLSS SDK, then build and package their platform's DLSS bridge and runtime libraries. The Windows job uses MSYS2 CLANG64 packages listed in [the Windows build guide](../packaging/windows/README.md) and a native Windows Python for PyInstaller. Intel XeSS remains an optional manual packaging step.
