#!/bin/zsh
# Scripted run into the first level, skipping the movies with clicks, then along a route: tools/walk.sh seconds "route input"
# The route's times count from when the level is playable (about 62 s after launch). Example: "2:down:0,3:up:0,3:down:13,60:up:13"
# It must not choose save slot 0, and scripted runs never write to Steam Cloud.
cd "$(dirname "$0")/.." || exit 1
secs=${1:-120}
IN=$(python3 -I -c "
import sys
base=62
route=[]
for item in '''${2:-}'''.split(','):
    if item:
        t,rest=item.split(':',1)
        route.append(f'{float(t)+base}:{rest}')
early=[f'{12+1.5*i}:press:0' for i in range(5)]
menu=['20:key:36','26:key:125','27:key:125','29:key:36','36:key:36','40:key:36']
late=[f'{42+1.5*i}:press:0' for i in range(13)]
print(','.join(early+menu+late+route))")
AUTO_PLAY=6 ARKHAM_FPS=1 ARKHAM_INPUT="$IN" NOTES=1 STUBS=1 FILES=1 FRAMES=16 exec tools/go.sh "$secs"
