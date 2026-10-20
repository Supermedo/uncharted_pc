"""Compile selected shadPS4/GoldHEN XML patches into out/patches.bin for the loader.

Patch addresses are PS4 virtual addresses (eboot base 0x400000); the loader's
image places eboot vaddr 0 at image offset 0. Only literal writes are supported
(bytes, bytes16/32/64, float32/64, utf8, utf16); pattern ("mask") patches are rejected.
"""
import argparse
import json
import os
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

EBOOT_BASE=0x400000
# BB_FPS presets: patch names from patches/Bloodborne.xml (app version 01.09).
FPS_PRESETS={'30':[],'60':['60 FPS++'],'90':['90 FPS++'],'uncap':['Uncap FPS++']}
# Upscaler presets (bbport.ini "preset", the in-game menu): output / render size ratio. The game
# then renders at 1920x1080 / ratio and the port's temporal upscaler restores the output size.
OUTPUT_SIZE=(1920,1080)
PRESET_SCALES=[1.0,1.5,1.7,2.0,3.0]
# The community patch changes two independent consumers: the game render/window setup
# and the UI movie viewport. Keep the latter at native size so glyph rasterisation and
# vector tessellation do not inherit the scene's FSR resolution.
RESOLUTION_TEMPLATE='Resolution Patch 1280x720 (16:9)'
# Effect switches (bbport.ini, in-game menu, launcher): key -> (patch when the key is 0, patch
# when it is 1). The game reads these at start; a change applies after a restart.
EFFECTS={
    'effect_chromatic_aberration':('Disable Chromatic Aberration',None),
    'effect_dof':('Disable DoF',None),
    'effect_motion_blur':('Disable Motion Blur (perf increase)',None),
    'effect_ssao':('Disable SSAO',None),
    'effect_game_aa':('Disable AA',None),
    'effect_dynamic_shadows':('Disable Dynamic Light Shadows (perf increase)',None),
    'effect_ssr':(None,'Enable Screen Space Reflections (READ NOTE)'),
    'skip_intro':(None,'Skip Intro'),
    'debug_camera':(None,'Restore Debug Camera'),
    'debug_menu':(None,'Restore Debug Menu (READ NOTES)'),
    # Cheats and gameplay tweaks (launcher "Cheats" page).
    'cheat_no_death':(None,'Player No Dead (Read note)'),
    'cheat_stealth':(None,'Player Stealth (Read note)'),
    'cheat_silent':(None,'Player Silent (Read note)'),
    'cheat_rally_no_decay':(None,'No Rally Decay'),
    'cheat_enemy_control':(None,'Enemy Control'),
    'tweak_no_rally':(None,'Disable Rally (HP Regain)'),
    'tweak_camera_distance':(None,'Increased camera distance'),
    'tweak_no_camera_rotation':(None,'Disable Camera Auto Rotation via Movement'),
    'tweak_easy_run':(None,'Sensitive Analog Input (easier to run)'),
    'tweak_ragdoll':(None,'DS1-like physics'),
}
# model_lod: -2 highest, 0 the game's, 1 lower, 2 lowest.
MODEL_LOD={'-2':'Model LOD -2 (Highest)','1':'Model LOD 1 (Lower)','2':'Model LOD 2 (Lowest)'}


def game_app_version(game):
    """APP_VER of the game folder's param.sfo ("01.09"), or None when it cannot be read."""
    try:
        from prepare import sfo
        return sfo((Path(game) / 'sce_sys/param.sfo').read_bytes()).get('APP_VER')
    except (OSError, ValueError, ImportError):
        return None


def game_title_id(game):
    """param.sfo's TITLE_ID ("CUSA02320", "CUSA03173", ...), or None when it cannot be read."""
    try:
        from prepare import sfo
        return sfo((Path(game) / 'sce_sys/param.sfo').read_bytes()).get('TITLE_ID')
    except (OSError, ValueError, ImportError):
        return None


def patch_title_ids(xml):
    """The Title IDs the patch database declares entries for.

    Takes the same argument compile_patches does: the database's path.
    """
    root = ET.parse(xml).getroot()
    return {node.text.strip() for group in root.iter('TitleID')
            for node in group.iter('ID') if node.text}


def check_patch_target(xml, game):
    """Why these patches must not be written to this game, or None when they may be.

    The app version alone is not enough. Patches are byte writes at the addresses of
    one game build, and two games can share an APP_VER ("01.09") while having entirely
    different code there. The database names the games it belongs to, so a game it does
    not list is refused even when the version matches -- otherwise a patch set for one
    game can land inside another game's executable.
    """
    title = game_title_id(game)
    known = patch_title_ids(xml)
    if not title or not known:
        return None  # cannot tell which game this is: the version check stands alone
    if title not in known:
        others = ', '.join(sorted(known)[:4])
        return (f'this game is {title}; the patches are for {others}... and none of '
                f'them belongs to it')
    return None


def game_versions(game):
    """The version a patch's AppVer must name for this dump, as a set of one: param.sfo's
    APP_VER ("01.02", the build that runs), or its VERSION (the content base version, "01.00")
    only when APP_VER is missing. Never both: a patch for the base version would be written
    into the running build's code. Empty when param.sfo cannot be read."""
    try:
        from prepare import sfo
        values = sfo((Path(game) / 'sce_sys/param.sfo').read_bytes())
    except (OSError, ValueError, ImportError):
        return set()
    version = values.get('APP_VER') or values.get('VERSION')
    return {version} if version else set()


def validate_patch_requirements(names, game):
    if 'Restore Debug Camera' in names and 'Enemy Control' in names:
        raise ValueError('Restore Debug Camera conflicts with Enemy Control; enable only one')
    if 'Restore Debug Menu (READ NOTES)' in names:
        font = game / 'dvdroot_ps4/font'
        missing = [name for name in ('DbgFont14h.ccm', 'DbgFont14h.tpf')
                   if not (font / name).is_file() or (font / name).stat().st_size == 0]
        if missing:
            raise ValueError('Debug menu needs non-empty font files in '
                             f'{font}: {", ".join(missing)}. Install the fonts from '
                             'https://www.nexusmods.com/bloodborne/mods/253 first; '
                             'or disable debug_menu in bbport.ini')


def effect_patches(settings):
    names=[]
    for key,(off,on) in EFFECTS.items():
        if key not in settings: continue
        name=on if settings[key]=='1' else off
        if name: names.append(name)
    # Enemy Control and the free camera use the same buttons: the free camera wins.
    if 'Enemy Control' in names and 'Restore Debug Camera' in names:
        names.remove('Enemy Control')
        print('Patches: Enemy Control skipped (it conflicts with the free camera)')
    # Rally that never decays means nothing without Rally.
    if 'No Rally Decay' in names and 'Disable Rally (HP Regain)' in names:
        names.remove('No Rally Decay')
    lod=MODEL_LOD.get(settings.get('model_lod','0'))
    if lod: names.append(lod)
    return names
SCENE_WIDTH=0x02196A6B-EBOOT_BASE
SCENE_HEIGHT=0x02196A7A-EBOOT_BASE
UI_WIDTH=0x02358554-EBOOT_BASE
UI_HEIGHT=0x0235855D-EBOOT_BASE


def read_settings(path):
    settings={}
    if path.exists():
        for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
            key,sep,value=line.partition('=')
            if sep and not line.startswith('#'): settings[key.strip()]=value.strip()
    return settings


def render_size(settings,override=''):
    """Render resolution for the upscaler preset, or None for native."""
    if override:
        w,h=(int(v) for v in override.lower().split('x'))
        return (w,h)
    if settings.get('upscaler','fsr3')=='off': return None
    preset=int(settings.get('preset','0') or 0)
    scale=PRESET_SCALES[max(0,min(preset,len(PRESET_SCALES)-1))]
    if scale==1.0: return None
    # Even sizes (the game has half-resolution buffers).
    return tuple(max(2,round(v/scale/2)*2) for v in OUTPUT_SIZE)


def output_size(settings):
    """Output (UI) size from bbport.ini output_res, e.g. 3840x2160; 1920x1080 by default."""
    try:
        w,h=(int(v) for v in settings.get('output_res','').lower().split('x'))
        if w>0 and h>0: return (w,h)
    except ValueError:
        pass
    return OUTPUT_SIZE


def scaled_sizes(settings):
    """(render, output) for an output other than 1080p (above it, or 720p for the Steam Deck):
    the game renders at output / preset scale (or at the output size without upscaler) and the
    upscaler fills the output. None at 1080p and for TAA (native, live host targets only)."""
    out=output_size(settings)
    if out==OUTPUT_SIZE or settings.get('upscaler')=='taa': return None
    scale=1.0
    if settings.get('upscaler','fsr3')!='off':
        preset=int(settings.get('preset','0') or 0)
        scale=PRESET_SCALES[max(0,min(preset,len(PRESET_SCALES)-1))]
    render=tuple(max(2,round(v/scale/2)*2) for v in out)
    # A scene of exactly 1920x1080 (4K Performance) is indistinguishable from the game's UI
    # coordinate space, which the port's UI composition recognizes by that size.
    if render==OUTPUT_SIZE: render=(1916,1078)
    return render,out


def resolution_writes(xml,size,app_version,segments,ui=OUTPUT_SIZE):
    writes=compile_patches(xml,[RESOLUTION_TEMPLATE],app_version,segments)
    replacements={SCENE_WIDTH:(0xB8,0x500,size[0]),
                  SCENE_HEIGHT:(0xB8,0x2D0,size[1]),
                  UI_WIDTH:(0xB8,0x500,ui[0]),
                  UI_HEIGHT:(0xB9,0x2D0,ui[1])}
    out=[]
    seen=set()
    for offset,data in writes:
        if offset in replacements:
            opcode,old,value=replacements[offset]
            if data!=bytes([opcode])+old.to_bytes(3,'little') or offset in seen:
                raise ValueError(f'unexpected resolution patch at {offset+EBOOT_BASE:#x}')
            data=bytes([opcode])+value.to_bytes(3,'little')
            seen.add(offset)
        out.append((offset,data))
    if seen!=replacements.keys():
        raise ValueError('resolution patch is missing scene/UI viewport instructions')
    return out


def eboot_segments(elf):
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    segments=[]
    for i in range(phnum):
        kind,_,_,vaddr,_,_,memsz,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        if kind==1: segments.append((vaddr,vaddr+memsz))
    return segments


def encode(line):
    kind,value=line.get('Type'),line.get('Value')
    if kind=='bytes': return bytes.fromhex(value.replace(' ',''))
    if kind in ('bytes16','bytes32','bytes64'):
        return int(value,0).to_bytes(int(kind[5:])//8,'little')
    if kind=='float32': return struct.pack('<f',float(value))
    if kind=='float64': return struct.pack('<d',float(value))
    if kind=='utf8': return value.encode()+b'\0'
    if kind=='utf16': return value.encode('utf-16-le')+b'\0\0'
    raise ValueError(f'unsupported patch type {kind!r}')


def compile_patches(xml, names, app_version, segments):
    found={}
    for meta in ET.parse(xml).getroot().iter('Metadata'):
        if meta.get('Name') in names and meta.get('AppVer')==app_version and meta.get('AppElf','eboot.bin')=='eboot.bin':
            found[meta.get('Name')]=meta
    missing=[n for n in names if n not in found]
    if missing: raise ValueError(f'patches not found for app version {app_version}: {missing}')
    writes=[]
    for name in names:
        for line in found[name].iter('Line'):
            offset=int(line.get('Address'),0)-EBOOT_BASE
            data=encode(line)
            if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                raise ValueError(f'{name}: address {line.get("Address")} is outside the eboot')
            writes.append((offset,data))
    return writes


# Third-party patch files (shadPS4/GoldHEN XML) in the data directory's patches/ folder.
BLOODBORNE_IDS={'CUSA00207','CUSA00208','CUSA00900','CUSA01363','CUSA03173','CUSA03023'}
# Uncharted: The Nathan Drake Collection (all three games share these title IDs).
NDC_IDS={'CUSA02320','CUSA02343','CUSA02344','CUSA02826'}
# The collection's executables, as patch files name them in AppElf (base 0x400000 for each).
NDC_ELFS=('eboot.bin','big2-ps4_Shipping.elf','big3-ps4_Shipping.elf')


def external_patches(directory, app_version='01.09', exclude=Path(__file__).resolve().parent.parent/'patches/Bloodborne.xml',
                     app_elf='eboot.bin', title_id=None, game=None, fail_closed=False):
    """[(key, file, metadata)] of patches for this executable and version in directory/*.xml.
    key is "<file name>/<patch name>" (the launcher's selection, patches.json). app_version may
    be one version or a set of them. With title_id (the dump's TITLE_ID), a file that lists title
    IDs must list that one. game ('bloodborne' or 'ndc'; inferred from title_id when not given)
    limits the files to those listing an ID of that game, so the collection's IDs are not offered
    to Bloodborne nor Bloodborne's to the collection. With fail_closed, an unknown title_id skips
    every file that lists title IDs instead of applying it."""
    if game is None and title_id:
        game='bloodborne' if title_id in BLOODBORNE_IDS else 'ndc' if title_id in NDC_IDS else None
    family={'bloodborne':BLOODBORNE_IDS,'ndc':NDC_IDS}.get(game,BLOODBORNE_IDS|NDC_IDS)
    versions={app_version} if isinstance(app_version,str) else set(app_version)
    found=[]
    directory=Path(directory)
    for path in sorted(directory.glob('*.xml')) if directory.is_dir() else []:
        if path.resolve()==Path(exclude).resolve(): continue  # the built-in file (patches/ in a checkout)
        try:
            root=ET.parse(path).getroot()
        except ET.ParseError as error:
            print(f'Patches: {path.name}: {error}',file=sys.stderr)
            continue
        ids={e.text.strip() for e in root.iter('ID') if e.text}
        if ids and not ids&family: continue
        if ids and fail_closed and not title_id: continue  # unknown game: never guess
        if title_id and ids and title_id not in ids: continue
        for meta in root.iter('Metadata'):
            if meta.get('AppVer') in versions and meta.get('AppElf','eboot.bin')==app_elf:
                found.append((f'{path.name}/{meta.get("Name")}',path,meta))
    return found


def external_selection(found, config):
    """Selected external patches: patches.json {"enabled": [...], "disabled": [...]} overrides
    each file's isEnabled."""
    settings={}
    if config and Path(config).is_file():
        settings=json.loads(Path(config).read_text(encoding='utf-8-sig'))
    enabled,disabled=set(settings.get('enabled',[])),set(settings.get('disabled',[]))
    return [(key,path,meta) for key,path,meta in found
            if key in enabled or (key not in disabled and meta.get('isEnabled','false').lower()=='true')]


def compile_external(selected, segments):
    """Writes of the selected external patches; a patch with unsupported lines is skipped whole."""
    writes=[]
    for key,_,meta in selected:
        try:
            ours=[]
            for line in meta.iter('Line'):
                offset=int(line.get('Address') or '',0)-EBOOT_BASE
                data=encode(line)
                if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                    raise ValueError(f'address {line.get("Address")} is outside the eboot')
                ours.append((offset,data))
        except ValueError as error:
            print(f'Patches: skipped {key}: {error}',file=sys.stderr)
            continue
        writes+=ours
        print(f'Patches: external {key} ({len(ours)} writes)')
    return writes


def write_patch_file(path, writes, append=False):
    """Write BBPATCH2 (base, then (offset, size, data) per write); with `append`, after the
    writes already in `path` when it holds a BBPATCH2 file with the same base."""
    before=[]
    if append and path.is_file():
        blob=path.read_bytes()
        if len(blob)>=24 and blob[:8]==b'BBPATCH2' and struct.unpack_from('<Q',blob,8)[0]==EBOOT_BASE:
            count,=struct.unpack_from('<Q',blob,16)
            at=24
            for _ in range(count):
                offset,size=struct.unpack_from('<QQ',blob,at); at+=16
                before.append((offset,blob[at:at+size])); at+=size
    writes=before+list(writes)
    blob=struct.pack('<8sQQ',b'BBPATCH2',EBOOT_BASE,len(writes))
    for offset,data in writes: blob+=struct.pack('<QQ',offset,len(data))+data
    path.write_bytes(blob)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--patches-dir',type=Path,help='third-party patch XML files (shadPS4 format)')
    p.add_argument('--patches-config',type=Path,help='patches.json: enabled/disabled external patches')
    p.add_argument('--xml',type=Path,default=Path(__file__).resolve().parent.parent/'patches/Bloodborne.xml')
    p.add_argument('--fps',choices=sorted(FPS_PRESETS),default='uncap')
    p.add_argument('--extra',default='',help='additional patch names, separated by ";"')
    p.add_argument('--app-version',default='01.09')
    p.add_argument('--out',type=Path,default=Path(__file__).resolve().parent.parent/'out')
    p.add_argument('--settings',type=Path,default=Path(__file__).resolve().parent.parent/'bbport.ini')
    p.add_argument('--game-dir',type=Path,default=Path(os.environ.get('BB_GAME_DIR','../CUSA03173')))
    p.add_argument('--render-res',default='',help='render resolution WxH (overrides the preset)')
    p.add_argument('--print-preset-size',action='store_true',help='print the selected preset size, if reduced')
    p.add_argument('--output-res',default='',help='output resolution WxH (the upscaler\'s; the UI stays 1920x1080)')
    p.add_argument('--print-scaled',action='store_true',
                   help='print "RENDER OUTPUT" (WxH) when bbport.ini selects an output other than 1080p')
    p.add_argument('--app-elf',default='eboot.bin',help='the executable the patches are for (AppElf)')
    p.add_argument('--external-only',action='store_true',
                   help='only the third-party patches for --app-elf and the dump\'s version (Uncharted)')
    a=p.parse_args()
    if a.external_only:
        writes=[]
        if a.patches_dir:
            found=external_patches(a.patches_dir,game_versions(a.game_dir),a.xml,a.app_elf,game_title_id(a.game_dir),fail_closed=True)
            writes=compile_external(external_selection(found,a.patches_config),
                                    eboot_segments((a.out/'eboot.elf').read_bytes()))
        # After the built-in patch set run.py compiled (apply_patches, empty for a game it does
        # not cover): appended to the same patches.bin, as the loader takes one file.
        write_patch_file(a.out/'patches.bin',writes,append=True)
        print(f'Patches: {a.app_elf}: {len(writes)} writes from external patches')
        return
    if a.print_scaled:
        sizes=scaled_sizes(read_settings(a.settings))
        if sizes:
            # Write the bytes: on Windows a pipe is in text mode, so print() (and
            # sys.stdout.write) emit CRLF, and the shell's `read` keeps the \r.
            # That would make BB_OUTPUT_RES "1280x720\r".
            sys.stdout.flush()
            os.write(sys.stdout.fileno(),
                     f'{sizes[0][0]}x{sizes[0][1]} {sizes[1][0]}x{sizes[1][1]}\n'.encode())
        return
    if a.print_preset_size:
        settings=read_settings(a.settings)
        if 'BB_UPSCALER' in os.environ:
            settings['upscaler']='fsr3' if os.environ['BB_UPSCALER']=='fsr3' else 'off'
        if 'BB_UPSCALE_PRESET' in os.environ:
            settings['preset']=os.environ['BB_UPSCALE_PRESET']
        size=render_size(settings)
        if size: print(f'{size[0]}x{size[1]}')
        return
    # The patches are byte writes at the addresses of one game version: on another version they
    # would corrupt code. Such a game runs unpatched (run.sh/run.py then choose 30 FPS).
    version=game_app_version(a.game_dir)
    declined=None
    if version and version!=a.app_version and not os.environ.get('BB_FORCE_PATCHES'):
        declined=f'game version {version}, patches are for {a.app_version}'
    else:
        # Right version is not the same game: the database says which games it is for.
        declined=check_patch_target(a.xml,a.game_dir)
    if declined:
        blob=struct.pack('<8sQQ',b'BBPATCH2',EBOOT_BASE,0)
        (a.out/'patches.bin').write_bytes(blob)
        print(f'Patches: {declined}: none applied '
              '(30 FPS, no effect or resolution patches)')
        return
    names=FPS_PRESETS[a.fps]+[n.strip() for n in a.extra.split(';') if n.strip()]
    names+=[n for n in effect_patches(read_settings(a.settings)) if n not in names]
    validate_patch_requirements(names,a.game_dir)
    segments=eboot_segments((a.out/'eboot.elf').read_bytes())
    writes=compile_patches(a.xml,names,a.app_version,segments)
    size=render_size(read_settings(a.settings),a.render_res) if a.render_res else None
    # The UI keeps the game's 1920x1080 coordinates even for a larger output: the port draws
    # it into the output-size image with a viewport scaled by output / 1920
    # (UiComposition::NativeViewport), so it is rasterized at the output resolution.
    ui=OUTPUT_SIZE
    if size:
        writes+=resolution_writes(a.xml,size,a.app_version,segments,ui)
        if size[0]*size[1]>OUTPUT_SIZE[0]*OUTPUT_SIZE[1]:
            heap='Increased Graphics Heap Sizes'
            writes+=compile_patches(a.xml,[heap],a.app_version,segments)
            names.append(heap)
        print(f'Patches: scene {size[0]}x{size[1]}; UI {ui[0]}x{ui[1]}')
    if a.patches_dir:
        # After the built-in ones: an external patch of the same bytes wins.
        writes+=compile_external(external_selection(external_patches(a.patches_dir,a.app_version,a.xml,
                                                          title_id=game_title_id(a.game_dir),game='bloodborne'),
                                                    a.patches_config),segments)
    # BBPATCH2: the patch base, so the loader can rebase pointers the patches write into
    # relocated slots (60/90 FPS++ replace function pointers).
    blob=struct.pack('<8sQQ',b'BBPATCH2',EBOOT_BASE,len(writes))
    for offset,data in writes: blob+=struct.pack('<QQ',offset,len(data))+data
    (a.out/'patches.bin').write_bytes(blob)
    print(f'Patches: FPS preset {a.fps}; {len(writes)} writes from {names or "none"}')


if __name__=='__main__':
    for _s in (sys.stdout, sys.stderr):  # console may be GBK etc.: titles carry a trademark sign
        if _s: _s.reconfigure(encoding='utf-8', errors='replace')
    main()
