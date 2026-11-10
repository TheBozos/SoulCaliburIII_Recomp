#!/usr/bin/env python3
"""The comparison routes: a pad script and the stills of each, in the frames of a port run WITH movies.

  routes.py list
  routes.py script NAME      the pad script (frame:BUTTON/hold,...)
  routes.py stills NAME      the frames to compare (comma separated)
  routes.py points NAME      `label frame` lines for compare.py

The scripts are the A/B ones (ab2/modes.sh's nine modes, reference/ab/route_ab.txt's fight), written for SC3_NO_MOVIES
where the title's press-start scene (0x6) begins at frame 915. With movies it begins at 3180, so every event and still
from 900 on moves by SHIFT. The fight route is played on several stages by the number of RIGHTs at stage select.
"""
import sys

SHIFT = 3180 - 915
TITLE = '1560:START/12,1740:START/12,1920:START/12,2100:START/12'
MODES = {'tos': (0, 0), 'chron': (0, 1), 'wc': (0, 2), 'arena': (0, 3), 'museum': (0, 5), 'cc': (0, 6), 'vs': (1, 0),
         'shop': (2, 0), 'options': (3, 0)}
FIGHT_HEAD = ('1560:START/12,1740:START/12,1920:START/12,2100:START/12,2640:CROSS/12,2880:DOWN/12,2940:DOWN/12,'
              '3000:DOWN/12,3060:DOWN/12,3180:CROSS/12,3420:DOWN/12,3540:CROSS/12,4020:CROSS/12,4260:CROSS/12,'
              '4500:CROSS/12,4740:CROSS/12,5220:RIGHT/12,5340:CROSS/12,5580:CROSS/12,5820:CROSS/12,6060:CROSS/12')
FIGHT_TAIL = ('7740:SQUARE/12,7800:TRIANGLE/12,7860:RIGHT/12,7920:SQUARE/12,7980:TRIANGLE/12,8040:RIGHT/12,'
              '8100:SQUARE/12,8160:TRIANGLE/12,8220:RIGHT/12,8280:SQUARE/12,8340:TRIANGLE/12,8400:RIGHT/12,'
              '8460:SQUARE/12,8520:TRIANGLE/12,8580:RIGHT/12,8640:SQUARE/12,8700:START/12')
STAGES = {'fight1': 3, 'fight2': 1, 'fight3': 6, 'fight4': 9}

BOOT = [('namco', 450), ('movie_a', 700), ('movie_b', 1300), ('movie_c', 2000), ('movie_d', 2500),
        ('produced', 2850), ('title_movie', 3100)]
MODE_STILLS = [('title', 1400), ('mode_select', 2300), ('single_player', 3000)] + \
    [(f'm{n}', n) for n in range(3600, 6801, 400)]
FIGHT_STILLS = [('charsel', 4300), ('stagesel', 6250), ('intro_a', 6700), ('intro_b', 7100), ('idle', 7500),
                ('attack_a', 7860), ('attack_b', 7920), ('attack_c', 7980), ('attack_d', 8040), ('attack_e', 8100),
                ('attack_f', 8160), ('attack_g', 8240), ('attack_h', 8360), ('pause_a', 8800), ('pause_b', 8810)]


def shift(frame):
    return frame + SHIFT if frame >= 900 else frame


def shifted(script):
    out = []
    for e in script.split(','):
        f, rest = e.split(':', 1)
        out.append(f'{shift(int(f))}:{rest}')
    return ','.join(out)


def mode_script(top, single):
    r, f = TITLE, 2400
    for _ in range(top):
        r += f',{f}:DOWN/12'
        f += 60
    r += ',2640:CROSS/12'
    f = 2880
    for _ in range(single):
        r += f',{f}:DOWN/12'
        f += 60
    return r + ''.join(f',{f}:CROSS/12' for f in range(3180, 6901, 240))


def script(name):
    if name in MODES:
        return shifted(mode_script(*MODES[name]))
    rights = ''.join(f',{6120 + 60 * i}:RIGHT/12' for i in range(STAGES[name]))
    cross = 6120 + 60 * STAGES[name]
    return shifted(f'{FIGHT_HEAD}{rights},{max(cross, 6300)}:CROSS/12,{FIGHT_TAIL}')


def points(name):
    pts = (BOOT if name == 'tos' else []) + (MODE_STILLS if name in MODES else FIGHT_STILLS)
    return [(label, shift(f) if (label, f) not in BOOT else f) for label, f in pts]


def main():
    cmd = sys.argv[1]
    if cmd == 'list':
        print(' '.join(list(MODES) + list(STAGES)))
    elif cmd == 'script':
        print(script(sys.argv[2]))
    elif cmd == 'stills':
        print(','.join(str(f) for _, f in points(sys.argv[2])))
    elif cmd == 'points':
        for label, f in points(sys.argv[2]):
            print(label, f)


if __name__ == '__main__':
    main()
