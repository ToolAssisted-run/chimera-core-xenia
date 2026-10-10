#!/bin/sh
# Applies the numbered patches to the extern/xenia submodule (Xenia Canary). Idempotent: a tree
# that already carries the whole series is left alone, a pristine one has it
# applied, and anything in between is an error that names the files.
#
# THE SERIES IS JUDGED AS A WHOLE, not a patch at a time: once two patches
# touch the same place, a fully patched tree neither applies nor reverses the
# earlier one, and a check that asks each patch in turn stops on a tree with
# nothing wrong with it.
#
# So: what the series SHOULD leave behind is worked out on a scratch copy - the
# touched files as the submodule's HEAD has them, with every patch applied in
# order - and the working tree is compared with that. It also catches the state
# the old check could not name: a tree where one file was reverted by hand
# (git checkout -- file), which silently drops that file's share of several
# patches and once produced a core with no rebuild machinery in it at all.
#
# The driver lives in waterbox/ and is built OUTSIDE the xenia tree, so nothing
# is copied in.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
# XENIA_TREE points it at another checkout: how this script is tested without
# touching the tree a build depends on
xenia="${XENIA_TREE:-$root/extern/xenia}"

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

# every file the series touches (a created or deleted file has /dev/null on one side)
cat "$root"/patches/*.patch | sed -n 's#^--- a/##p; s#^+++ b/##p' | sort -u > "$scratch/touched"

# the scratch copy: those files as HEAD has them (one the series creates is absent)
mkdir "$scratch/tree"
while IFS= read -r f; do
	if git -C "$xenia" cat-file -e "HEAD:$f" 2>/dev/null; then
		mkdir -p "$scratch/tree/$(dirname "$f")"
		git -C "$xenia" show "HEAD:$f" > "$scratch/tree/$f"
		# the mode travels too, or git apply remarks on every executable file
		case "$(git -C "$xenia" ls-tree HEAD -- "$f")" in 100755*) chmod +x "$scratch/tree/$f" ;; esac
	fi
done < "$scratch/touched"

# pristine: the working tree still has HEAD's version of every touched file
pristine=1
while IFS= read -r f; do
	if [ -e "$scratch/tree/$f" ]; then
		cmp -s "$scratch/tree/$f" "$xenia/$f" || { pristine=0; break; }
	elif [ -e "$xenia/$f" ]; then
		pristine=0; break
	fi
done < "$scratch/touched"

# The series is tried on the scratch copy FIRST, whichever state the tree is in:
# a series that does not apply in order must be found out here, and not half
# way through applying it to the real tree.
for p in "$root"/patches/*.patch; do
	(cd "$scratch/tree" && git apply "$p") || {
		echo "the series does not apply to the submodule's HEAD at $(basename "$p") - was the pin moved without rebasing the patches?" >&2
		exit 1
	}
done


if [ "$pristine" -eq 1 ]; then
	for p in "$root"/patches/*.patch; do
		git -C "$xenia" apply "$p"
		echo "applied: $(basename "$p")"
	done
	exit 0
fi

# not pristine: it must then be EXACTLY what the whole series leaves behind
wrong=0
while IFS= read -r f; do
	if [ -e "$scratch/tree/$f" ]; then
		cmp -s "$scratch/tree/$f" "$xenia/$f" 2>/dev/null || { echo "not as the series leaves it: $f" >&2; wrong=1; }
	elif [ -e "$xenia/$f" ]; then
		echo "the series deletes this, and it is there: $f" >&2; wrong=1
	fi
done < "$scratch/touched"

if [ "$wrong" -ne 0 ]; then
	echo "extern/xenia is partly patched. To start again from the submodule's HEAD:" >&2
	echo "  git -C extern/xenia reset --hard && git -C extern/xenia clean -fd && waterbox/apply-patches.sh" >&2
	echo "(that discards edits made in the tree - turn them into a patch first)" >&2
	exit 1
fi
echo "already applied: all $(ls "$root"/patches/*.patch | wc -l) patches"
