"""Require a published Git revision before PlatformIO uploads firmware."""

import subprocess

from SCons.Errors import UserError
from SCons.Script import COMMAND_LINE_TARGETS

Import("env")


def git(project, *args, capture=False):
    try:
        return subprocess.run(
            ["git", *args],
            cwd=project,
            text=True,
            stdout=subprocess.PIPE if capture else None,
            check=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError) as error:
        raise UserError(f"Git {args[0]} failed. Fix Git or GitHub access before uploading.") from error


def clean_revision(project):
    if git(project, "status", "--porcelain", "--untracked-files=all", capture=True).strip():
        raise UserError("Commit the project changes before uploading firmware.")
    branch = git(project, "branch", "--show-current", capture=True).strip()
    if not branch:
        raise UserError("Check out a Git branch before uploading firmware.")
    return branch, git(project, "rev-parse", "HEAD", capture=True).strip()


if "upload" in COMMAND_LINE_TARGETS:
    project = env.subst("$PROJECT_DIR")
    build_revision = clean_revision(project)

    def before_upload(source, target, env):
        branch, revision = clean_revision(project)
        if (branch, revision) != build_revision:
            raise UserError("The checked-out revision changed during the build. Run upload again.")
        git(project, "push", "--porcelain", "origin", f"{revision}:refs/heads/{branch}")
        print(f"GitHub has {branch} at {revision[:12]}. Uploading firmware.")

    env.AddPreAction("upload", before_upload)
