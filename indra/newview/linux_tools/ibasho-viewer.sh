#!/bin/bash
# Start the Ibasho viewer from its folder (the Ibasho tools start bin/ibasho-viewer-bin the same way themselves).
# Ibasho viewer source code: an addition to the Second Life Viewer Source Code (Linden Research, Inc.), written
# for Ibasho by Claude (Anthropic) models; GNU LGPL 2.1 only, as the rest of the viewer.
cd "$(dirname "$(readlink -f "$0")")" || exit 1
export mesa_glthread=true
export LD_LIBRARY_PATH="$PWD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec bin/ibasho-viewer-bin "$@"
