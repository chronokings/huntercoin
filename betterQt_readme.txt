Pending Transaction Monitor
===========================

  The "pending tx monitor" can reroute output from the console command "name_pending" to GUI

  input:
    middle mouse button       switch tx monitor on (default update interval: 5 seconds)

    name_pending 5            "legacy" console command to start with update interval every 5 seconds
                              (min update interval is 2 seconds, console is not available while it runs)

    middle mouse button       stop tx monitor

    Ctrl+middle mouse button  toggle silent mode

    names.txt                 up to 30 player names of "friendly" hunters + distance to trigger alarm. Changes are effective after stopping and restarting the tx monitor.
                              - if name is in names.txt, hunters ignore each other for alarm purposes
                              - it's not required that all friendlies are controlled by the same node/wallet

  output:
    small_wave_file.wav      played (by asking the OS to open it) on alarm, must be in same folder as debug.log (not included, windows system sounds work just fine)

    *ALARM*: <name> [<name>]  after all names from names.txt in case of alarm
    <n> min: <name>           after all names from names.txt, longest idle hunter (out of waypoints for n minutes)
    (OK)                      after all names from names.txt, in case of no alarm and no idle hunter
    x,y->x2,y2               after hunter name, coors currently and expected after next block
    wp:...                   after hunter name, next waypoint
    tx*<age>:...             after hunter name, value of pendig tx, and how "old" it is
    CONTACT*<age>            we will be in destruct range with an enemy player after next block. If started with update interval of 5 seconds,
                             then "CONTACT*4" or "CONTACT*5" (20 or 25 seconds) is basically seeing the white in their eyes


Asciiartmap editing guide
=========================

note: Up to 7 map tiles can be placed on top of each other (1 terrain layer, 3 shadow layers, and 3 normal layers
      for map objects like trees, cliffs or palisades)

      It's possible that the number of layers is insufficient (e.g. dense forest). In this case all tiles for 1 entire map object
      will be skipped. If only the number of shadow layers is insufficient, 1 visually unimportant part of the shadow will be skipped,
      but the map object is rendered.


Terrain
-------

0   grass terrain
1   grass terrain, unwalkable (no difference to '0', because hardcoded ObstacleMap is used to determine whether a tile is walkable)

.   "dirt" terrain

w   muddy water
W   blue water

O   cobblestone
o   cobblestone
Q   cobblestone
q   cobblestone
8   cobblestone


    grass terrain tiles adjacent to '.' are automatically painted with grass/dirt transition tiles

    "dirt" terrain tiles adjacent to cobblestone are automatically painted with cobblestone/dirt transition tiles


Grass
-----

"   high green grass (semi-random offset)
'   same as above, but green-to-yellow
v   same as above, but red (2 versions chosen at random)

    these grass tiles have their own small shadow baked in and (unlike boulders) don't use up shadow layers


Trees
-----

B   broadleaf, dark
b   broadleaf, bright
C   conifer, dark
c   conifer, bright

00000
00000
001B0       <- broadleaf tree: the 'B' tile is 1 tile right of the "unwalkable" center
00000
001C0       <- conifer tree: 2 unwalkable tiles
00000


Trees (or rocks, or high grass) stand visually on "dirt terrain" if
the tile under the tree is dirt, otherwise they would stand on grass terrain.

00000000000
000H0G00000    <- menhir and boulder on grass terrain
00B00G00"00    <- tree, boulder and high grass on dirt terrain
00.00.00.00
00000000000


Rocks
-----

G   boulder, dark
g   boulder, bright
H   menhir, dark
h   menhir, bright


Palisades + Gate
------------------

P   palisade, bird looking left
p   palisade, bird looking right
U   gate


Terrain (on cliff)
------------------

;   sand terrain on cliffs
;   alternative version
,   alternative version

    grass terrain tiles adjacent to ';', ':' or ',' are automatically painted with grass/sand transition tiles
    (unfinished, expect glitches)

+   filler (painted as grass, but normally not visible)
#   filler


Cliffs
------

cliff from CLIFFVEG.bmp

  v-v-------- 2 versions of normal "column" ('!', '?' and '|', '_') can be mixed randomly

           v---- conifer tree, need 1 tile distance to cliff (if on right side)

             v----- broadleaf tree, need 2 tiles distance to cliff (if on right side)
1?__??_?_1
1:;,:;,:,1                                                              <----  3 versions of cliff "dirt" (';', ':' and ',') can be mixed randomly
<;:;,:;;:> C       <---- special version of "line" ('<', '>')           <----
(,:,,::,;)                                                              <----
{++++++++}  B      <--- 2 versions of normal "line" ('(', ')' and '{', '}') can be mixed randomly
(++++++++)         <---
[!||!!|!|]C
  B   C            <--- trees

inverse cliff (cliff corners from CLIFVEG2.bmp)

 l|!|!||r
1000000001
}00000000(
)00000000{
}00000000(
)00000000(
)L_?_?__R{


cliff with special pieces from CLIFVEG2.bmp

                               ##?##
1?__??_?_1    1?__??_?_1    1?_#/?#\_1
1:;,:;,:,1    1:;,:;,:,1    1:;,:;,:,1
<;:;,:;;:>    <;:;,:;;:>    <;:;,:;;:>
(,:,,::,;)    (,:,,::,;)    (,:,,::,;)
{++++++++}    {++++:+++}    {++++++++}
(++++++++)    (++++,+++)    (++++++++)
[!|##!##|]    [!|##+##|]    [!||!!|!|]
   #S!#s         ##+##
                 #Z!#z


grass tiles on cliff        Trees on cliff stand visually on "sand" if
need adjacent               the tile under the tree is sand, otherwise they would stand on grass terrain.
"sand on cliff" tiles       (same for rocks or high grass)

                              C B
1?__??_?_1                  1?__??_?_1
1:;,:;,:,1                  1:;,C;B:,1
<;000000:>                  <;:;,:;;:>
(,000000;)                  iC:c,::,;J
{,00000;+}                  I;+;++++Cj
(+,:;++++)                 Bi+++++++;J
[!|##!##|]                  m!||!!|!|]
   #S!#s
                                     ^-- right side of cliff: tiles painted in terrain layer if 'J' and 'j' is used instead of ')' and '}'
                                         (otherwise shadow tiles from the adjacent tree would not be visible)

                            ^-- left side of cliff: tiles painted in terrain layer if 'I', 'i' and 'm' is used instead of '(' and '{' and '['
                                (otherwise parts of the adjacent conifer tree and its shadow would not be visible)

