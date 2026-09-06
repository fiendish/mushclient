"""Check for LuaCOM updates and publish new, owner-verified draft pull requests."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

import luacom_vendor as vendor


HOST = "fiendish/mushclient"
OWNER = "fiendish"
BASE = "master"


def gh(*arguments):
    return subprocess.check_output(["gh", *arguments]).decode().strip()


def api(endpoint):
    return json.loads(gh("api", endpoint))


def identity():
    if api("user")["login"] != OWNER:
        raise ValueError("LUACOM_SYNC_TOKEN must authenticate as fiendish")


def upstream_revision():
    return vendor.revision(api("repos/fiendish/luacom/commits/master")["sha"])


def pull_requests(branch):
    pages = json.loads(gh("api", "--paginate", "--slurp",
                         f"repos/{HOST}/pulls?state=all&head={OWNER}:{branch}&per_page=100"))
    result = [pr for page in pages for pr in page]
    for pr in result:
        if pr["head"]["ref"] != branch or pr["head"]["repo"]["full_name"] != HOST:
            raise ValueError("Unexpected pull request in the exact-head search")
    return result


def owned(pr, branch):
    if (pr["user"]["login"] != OWNER or pr["head"]["ref"] != branch
            or pr["head"]["repo"]["full_name"] != HOST
            or pr["base"]["repo"]["full_name"] != HOST or pr["base"]["ref"] != BASE):
        raise ValueError("Pull request ownership or branch does not match")


def remote_head(root, branch):
    output = vendor.git(root, "ls-remote", "--heads", "origin", "refs/heads/" + branch).decode().strip()
    if not output:
        return None
    sha, ref = output.split()
    if ref != "refs/heads/" + branch:
        raise ValueError("Unexpected remote branch")
    return vendor.revision(sha)


def new_target(root, branch, expected_head=None):
    identity()
    verify_remote(root)
    if pull_requests(branch):
        raise ValueError("The proposed branch already has a pull request")
    if remote_head(root, branch) != expected_head:
        raise ValueError("The proposed remote branch has an unexpected head")


def verify_remote(root):
    allowed = {f"https://github.com/{HOST}.git", f"https://github.com/{HOST}",
               f"git@github.com:{HOST}.git"}
    for options in (("--all",), ("--push", "--all")):
        urls = vendor.git(root, "remote", "get-url", *options, "origin").decode().splitlines()
        if len(urls) != 1 or urls[0] not in allowed:
            raise ValueError("The effective origin fetch and push URLs must identify fiendish/mushclient")


def committed_files(root, commit, prefix):
    result = {}
    for entry in vendor.git(root, "ls-tree", "-r", "-z", commit, "--", prefix).split(b"\0"):
        if entry:
            metadata, name = entry.split(b"\t", 1)
            mode, kind, oid = metadata.split()
            if kind != b"blob" or mode not in {b"100644", b"100755"}:
                raise ValueError("Unexpected file type in the update pull request")
            result[name.decode().removeprefix(prefix + "/")] = vendor.VendorFile(
                mode.decode(), vendor.git(root, "cat-file", "blob", oid.decode()))
    return result


def existing_update(root, branch, target, pin, prs):
    if len(prs) != 1:
        raise ValueError("Multiple pull requests exist for the update branch")
    pr = prs[0]
    owned(pr, branch)
    if pr["state"] != "open":
        raise ValueError("The update branch has a closed pull request; review it before retrying")
    head = vendor.revision(pr["head"]["sha"])
    if remote_head(root, branch) != head:
        raise ValueError("The update pull request head changed")
    vendor.git(root, "fetch", "--quiet", "--no-tags", "--depth=1", "origin", head)
    actual_pin = json.loads(vendor.git(root, "show", head + ":" + str(vendor.PIN)))
    candidate_pin = dict(pin, revision=target)
    if actual_pin != candidate_pin:
        raise ValueError("The existing update pull request has a different manifest")
    if committed_files(root, head, "third_party/luacom/patches") != committed_files(root, "HEAD", "third_party/luacom/patches"):
        raise ValueError("The existing update pull request has different host patches")
    with vendor.source_repository(target) as source:
        expected = vendor.reconstruct(root, candidate_pin, source)
    if committed_files(root, head, "luacom") != expected:
        raise ValueError("The existing update pull request has source drift")
    print("Existing LuaCOM update: " + pr["html_url"])


def publish(root, target):
    identity()
    verify_remote(root)
    target = vendor.revision(target)
    pin = vendor.read_pin(root)
    vendor.run(root, "check", index=True)
    if target != upstream_revision():
        raise ValueError("LuaCOM master changed after detection; run the update again")
    if target == pin["revision"]:
        print("LuaCOM is current")
        return
    if vendor.git(root, "status", "--porcelain", "--untracked-files=all").strip():
        raise ValueError("The update requires a clean checkout")
    base = vendor.git(root, "rev-parse", "HEAD").decode().strip()
    if base != api(f"repos/{HOST}/commits/{BASE}")["sha"]:
        raise ValueError("The update requires the current MUSHclient master commit")
    branch = "update-luacom-" + target
    prs = pull_requests(branch)
    if prs:
        existing_update(root, branch, target, pin, prs)
        return
    new_target(root, branch)
    vendor.git(root, "switch", "--create", branch)
    vendor.run(root, "update", new_revision=target)
    vendor.git(root, "add", "--", "luacom", str(vendor.PIN))
    vendor.run(root, "check", index=True)
    subject = "Update LuaCOM to " + target[:12]
    vendor.git(root, "-c", "user.name=fiendish", "-c", "user.email=201996+fiendish@users.noreply.github.com",
               "commit", "--message", subject)
    if vendor.git(root, "log", "-1", "--format=%s").decode().strip() != subject:
        raise ValueError("Unexpected update commit subject")
    head = vendor.git(root, "rev-parse", "HEAD").decode().strip()
    changed = vendor.git(root, "diff", "--name-only", base, head).decode().splitlines()
    if not changed or any(not name.startswith("luacom/") and name != str(vendor.PIN) for name in changed):
        raise ValueError("The update commit includes unexpected paths")
    new_target(root, branch)
    if base != api(f"repos/{HOST}/commits/{BASE}")["sha"]:
        raise ValueError("MUSHclient master changed while preparing the update")
    vendor.git(root, "-c", "credential.helper=", "-c",
               "credential.https://github.com.helper=!gh auth git-credential",
               "push", "origin", "HEAD:refs/heads/" + branch)
    new_target(root, branch, head)
    body = (f"Import LuaCOM [{target[:12]}](https://github.com/fiendish/luacom/commit/{target}) "
            "and reapply the declared MUSHclient patches. Preserve the exact upstream file inventory, "
            "bytes, and modes except for those explicit host changes.\n")
    with tempfile.TemporaryDirectory(prefix="luacom-pr-") as temporary:
        path = Path(temporary) / "body.md"
        path.write_text(body)
        url = gh("pr", "create", "--repo", HOST, "--base", BASE, "--head", OWNER + ":" + branch,
                 "--draft", "--title", subject, "--body-file", str(path))
    match = re.fullmatch(r"https://github.com/fiendish/mushclient/pull/([0-9]+)", url)
    if not match:
        raise ValueError("Unexpected created pull request URL: " + url)
    identity()
    owned(api(f"repos/{HOST}/pulls/{match[1]}"), branch)
    print("Created LuaCOM update: " + url)


def check(root):
    vendor.run(root, "check", index=True)
    target = upstream_revision()
    changed = target != vendor.read_pin(root)["revision"]
    print("LuaCOM update available" if changed else "LuaCOM is current")
    if "GITHUB_OUTPUT" in os.environ:
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
            output.write(f"update={str(changed).lower()}\nrevision={target}\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["check", "publish"])
    parser.add_argument("--revision")
    args = parser.parse_args()
    if bool(args.revision) != (args.action == "publish"):
        parser.error("--revision is required only for publish")
    if args.action == "check":
        check(vendor.ROOT)
    else:
        publish(vendor.ROOT, args.revision)


if __name__ == "__main__":
    main()
