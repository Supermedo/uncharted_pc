from paths import ROOT
import struct
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest.mock import patch

import patches
from patches import (EBOOT_BASE, OUTPUT_SIZE, RESOLUTION_TEMPLATE, SCENE_HEIGHT,
                     SCENE_WIDTH, UI_HEIGHT, UI_WIDTH, compile_patches,
                     render_size, resolution_writes, scaled_sizes, effect_patches,
                     validate_patch_requirements, external_patches, external_selection,
                     compile_external)

XML = ROOT / 'patches/Bloodborne.xml'
SEGMENTS = [(0, 0x6000000)]


def immediate(writes, address):
    # The XML splits a 9-byte MOV imm32 + NOP into 4+4+1 byte writes.
    data = bytearray(9)
    for offset, value in writes:
        if address <= offset < address + 9:
            data[offset-address:offset-address+len(value)] = value
    assert data[5:] == b'\x0f\x1f\x40\x00'
    return data[0], struct.unpack_from('<I', data, 1)[0]


class NativeUiTests(unittest.TestCase):
    def test_presets_keep_ui_native(self):
        for preset, expected in [(1, (1280, 720)), (2, (1130, 636)),
                                 (3, (960, 540)), (4, (640, 360))]:
            with self.subTest(preset=preset):
                size = render_size({'upscaler': 'fsr3', 'preset': str(preset)})
                self.assertEqual(size, expected)
                writes = resolution_writes(XML, size, '01.09', SEGMENTS)
                self.assertEqual(immediate(writes, SCENE_WIDTH), (0xB8, size[0]))
                self.assertEqual(immediate(writes, SCENE_HEIGHT), (0xB8, size[1]))
                self.assertEqual(immediate(writes, UI_WIDTH), (0xB8, OUTPUT_SIZE[0]))
                self.assertEqual(immediate(writes, UI_HEIGHT), (0xB9, OUTPUT_SIZE[1]))

    def test_explicit_scene_resolution_keeps_native_ui(self):
        writes = resolution_writes(XML, (800, 450), '01.09', SEGMENTS)
        self.assertEqual(immediate(writes, SCENE_WIDTH)[1], 800)
        self.assertEqual(immediate(writes, UI_WIDTH)[1], 1920)
        self.assertEqual(immediate(writes, UI_HEIGHT)[1], 1080)

    def test_coordinate_and_aspect_fixes_are_preserved(self):
        original = compile_patches(XML, [RESOLUTION_TEMPLATE], '01.09', SEGMENTS)
        modified = resolution_writes(XML, (960, 540), '01.09', SEGMENTS)
        self.assertEqual(len(original), len(modified))
        changed = {SCENE_WIDTH, SCENE_HEIGHT, UI_WIDTH, UI_HEIGHT}
        self.assertEqual([w for w in original if w[0] not in changed],
                         [w for w in modified if w[0] not in changed])

    def test_unexpected_or_missing_ui_instruction_is_rejected(self):
        for remove in (False, True):
            tree = ET.parse(XML)
            for meta in tree.getroot().iter('Metadata'):
                if meta.get('Name') == RESOLUTION_TEMPLATE and meta.get('AppVer') == '01.09':
                    patch_list = meta.find('PatchList')
                    for line in list(patch_list):
                        if int(line.get('Address'), 0) == UI_WIDTH + EBOOT_BASE:
                            if remove:
                                patch_list.remove(line)
                            else:
                                line.set('Value', '0x000500B9')
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / 'patch.xml'
                tree.write(path)
                with self.assertRaises(ValueError):
                    resolution_writes(path, (960, 540), '01.09', SEGMENTS)

    def test_native_and_disabled_upscaler_need_no_resolution_patch(self):
        self.assertIsNone(render_size({'preset': '0'}))
        self.assertIsNone(render_size({'upscaler': 'off', 'preset': '3'}))

    def test_output_other_than_1080p_scales_the_scene(self):
        self.assertIsNone(scaled_sizes({'output_res': '1920x1080', 'preset': '2'}))
        # Steam Deck: below 1080p the scene is still the preset's fraction of the output.
        for preset, expected in [(0, (1280, 720)), (2, (752, 424)), (4, (426, 240))]:
            with self.subTest(preset=preset):
                self.assertEqual(scaled_sizes({'output_res': '1280x720', 'preset': str(preset)}),
                                 (expected, (1280, 720)))
        self.assertEqual(scaled_sizes({'output_res': '3840x2160', 'preset': '3'}),
                         ((1916, 1078), (3840, 2160)))
        # TAA is native-only and uses the live host targets.
        self.assertIsNone(scaled_sizes({'output_res': '1280x720', 'upscaler': 'taa', 'preset': '3'}))


class DebugPatchTests(unittest.TestCase):
    def test_camera_patch_is_optional_and_compatible_with_fps_and_debug_menu(self):
        self.assertEqual(effect_patches({'debug_camera': '0', 'debug_menu': '0'}), [])
        camera = effect_patches({'debug_camera': '1'})
        writes = compile_patches(XML, camera, '01.09', SEGMENTS)
        self.assertGreater(len(writes), 0)
        camera_bytes = {offset+i: byte for offset, data in writes for i, byte in enumerate(data)}
        for patch in ('Uncap FPS++', '60 FPS++', '90 FPS++', 'Restore Debug Menu (READ NOTES)'):
            for offset, data in compile_patches(XML, [patch], '01.09', SEGMENTS):
                for i, byte in enumerate(data):
                    if offset+i in camera_bytes:
                        self.assertEqual(camera_bytes[offset+i], byte, patch)

    def test_debug_menu_checks_both_fonts_but_camera_does_not_need_them(self):
        names = effect_patches({'debug_menu': '1'})
        with tempfile.TemporaryDirectory() as directory:
            game = Path(directory)
            validate_patch_requirements(['Restore Debug Camera'], game)
            with self.assertRaisesRegex(ValueError, 'DbgFont14h.ccm.*DbgFont14h.tpf'):
                validate_patch_requirements(names, game)
            font = game / 'dvdroot_ps4/font'
            font.mkdir(parents=True)
            (font / 'DbgFont14h.ccm').write_bytes(b'test')
            (font / 'DbgFont14h.tpf').touch()
            with self.assertRaisesRegex(ValueError, 'DbgFont14h.tpf'):
                validate_patch_requirements(names, game)
            (font / 'DbgFont14h.tpf').write_bytes(b'test')
            validate_patch_requirements(names, game)

    def test_conflicting_enemy_control_patch_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'conflicts with Enemy Control'):
            validate_patch_requirements(['Enemy Control', 'Restore Debug Camera'], Path('.'))


EXTERNAL = """<?xml version="1.0"?>
<Patch>
  <TitleID><ID>CUSA03173</ID><ID>CUSA00207</ID></TitleID>
  <Metadata Title="Bloodborne" Name="On" Author="x" PatchVer="1.0" AppVer="01.09" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00401000" Value="9090"/></PatchList>
  </Metadata>
  <Metadata Title="Bloodborne" Name="Off" Author="x" PatchVer="1.0" AppVer="01.09" AppElf="eboot.bin">
    <PatchList><Line Type="bytes32" Address="0x00402000" Value="0x12345678"/></PatchList>
  </Metadata>
  <Metadata Title="Bloodborne" Name="Mask" Author="x" PatchVer="1.0" AppVer="01.09" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="mask" Value="90 ?? 90" Offset="0"/></PatchList>
  </Metadata>
  <Metadata Title="Bloodborne" Name="Old" Author="x" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00403000" Value="90"/></PatchList>
  </Metadata>
</Patch>"""


class ExternalPatchTests(unittest.TestCase):
    def test_selection_and_unsupported_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / 'extra.xml').write_text(EXTERNAL)
            (folder / 'other.xml').write_text(EXTERNAL.replace('CUSA03173', 'CUSA99999')
                                              .replace('CUSA00207', 'CUSA99998'))
            (folder / 'broken.xml').write_text('<Patch>')
            found = external_patches(folder)
            self.assertEqual([key for key, _, _ in found],
                             ['extra.xml/On', 'extra.xml/Off', 'extra.xml/Mask'])
            # The file's isEnabled; the mask patch is skipped as a whole.
            writes = compile_external(external_selection(found, None), SEGMENTS)
            self.assertEqual(writes, [(0x1000, bytes.fromhex('9090'))])
            config = folder / 'patches.json'
            config.write_text('{"enabled": ["extra.xml/Off"], "disabled": ["extra.xml/On"]}')
            writes = compile_external(external_selection(found, config), SEGMENTS)
            self.assertEqual(writes, [(0x2000, (0x12345678).to_bytes(4, 'little'))])

    def test_collection_patches_follow_the_executable_and_version(self):
        # Synthetic file in the shadPS4 format (no game data): one patch per executable.
        ndc = """<?xml version="1.0"?>
<Patch>
  <TitleID><ID>CUSA02320</ID><ID>CUSA02343</ID></TitleID>
  <Metadata Title="NDC" Name="One" Author="x" PatchVer="1.0" AppVer="01.02" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00401000" Value="90"/></PatchList>
  </Metadata>
  <Metadata Title="NDC" Name="Two" Author="x" PatchVer="1.0" AppVer="01.02" AppElf="big2-ps4_Shipping.elf" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00402000" Value="90"/></PatchList>
  </Metadata>
  <Metadata Title="NDC" Name="Three" Author="x" PatchVer="1.0" AppVer="01.00" AppElf="big3-ps4_Shipping.elf" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00403000" Value="90"/></PatchList>
  </Metadata>
</Patch>"""
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / 'ndc.xml').write_text(ndc)
            versions = {'01.02', '01.00'}  # the dump's VERSION and APP_VER
            for elf, name, offset in (('eboot.bin', 'One', 0x1000),
                                      ('big2-ps4_Shipping.elf', 'Two', 0x2000),
                                      ('big3-ps4_Shipping.elf', 'Three', 0x3000)):
                found = external_patches(folder, versions, app_elf=elf)
                self.assertEqual([key for key, _, _ in found], [f'ndc.xml/{name}'])
                writes = compile_external(external_selection(found, None), SEGMENTS)
                self.assertEqual(writes, [(offset, b'\x90')])
            # Another version of the dump gets nothing; Bloodborne's default is unchanged.
            self.assertEqual(external_patches(folder, {'01.09'}, app_elf='eboot.bin'), [])
            self.assertEqual(external_patches(folder), [])

    def test_a_patch_for_one_game_does_not_reach_another(self):
        # The Mac's case (2026-10-18): a U1-only eboot.bin patch, and Uncharted 2 dumped on its
        # own, run from its folder's eboot.bin under another title ID.
        u1_only = """<?xml version="1.0"?>
<Patch>
  <TitleID><ID>CUSA02320</ID></TitleID>
  <Metadata Title="NDC" Name="U1 only" Author="x" PatchVer="1.0" AppVer="01.02" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00401000" Value="90"/></PatchList>
  </Metadata>
</Patch>"""
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / 'u1.xml').write_text(u1_only)
            # run.py names the game being run, not the file it found (see test_run_patches).
            self.assertEqual(external_patches(folder, {'01.02'}, app_elf='big2-ps4_Shipping.elf'), [])
            # A dump under a title ID the file does not list gets nothing either.
            self.assertEqual(external_patches(folder, {'01.02'}, app_elf='eboot.bin', title_id='CUSA02343'), [])
            # The collection's own U1 still gets it.
            self.assertEqual(len(external_patches(folder, {'01.02'}, app_elf='eboot.bin', title_id='CUSA02320')), 1)

    def test_built_in_file_is_not_external(self):
        self.assertEqual(external_patches(XML.parent), [])


class PatchTargetTests(unittest.TestCase):
    """A patch database must never be written into a game it does not belong to."""

    def test_title_ids_are_read_from_the_database(self):
        ids = patches.patch_title_ids(XML)
        self.assertIn('CUSA03173', ids)          # Bloodborne
        self.assertNotIn('CUSA02344', ids)       # Uncharted

    def test_a_game_the_database_does_not_list_is_refused(self):
        # Same APP_VER is not the same game: Bloodborne 01.09 and a hypothetical
        # Uncharted 01.09 have different code at these addresses.
        with patch.object(patches, 'game_title_id', return_value='CUSA02344'):
            self.assertIsNotNone(patches.check_patch_target(XML, '/game'))

    def test_the_game_the_database_is_for_is_allowed(self):
        with patch.object(patches, 'game_title_id', return_value='CUSA03173'):
            self.assertIsNone(patches.check_patch_target(XML, '/game'))

    def test_an_unreadable_title_id_does_not_block(self):
        # Nothing to compare against: the version check has to stand alone rather
        # than refusing every game whose param.sfo cannot be read.
        with patch.object(patches, 'game_title_id', return_value=None):
            self.assertIsNone(patches.check_patch_target(XML, '/game'))


def synthetic_sfo(fields):
    """A minimal param.sfo with these string fields (synthetic, no game data)."""
    keys = b''.join(k.encode() + b'\0' for k in fields)
    values = b''.join(v.encode().ljust(16, b'\0') for v in fields.values())
    entries, key_at, value_at = b'', 0, 0
    for key, value in fields.items():
        entries += struct.pack('<HHIII', key_at, 0x204, len(value) + 1, 16, value_at)
        key_at += len(key) + 1
        value_at += 16
    keys_offset = 20 + 16 * len(fields)
    values_offset = (keys_offset + len(keys) + 3) & ~3
    keys = keys.ljust(values_offset - keys_offset, b'\0')
    return struct.pack('<4sIIII', b'\0PSF', 0x101, keys_offset, values_offset, len(fields)) \
        + entries + keys + values


def dump_with(root, fields):
    (root / 'sce_sys').mkdir(parents=True, exist_ok=True)
    (root / 'sce_sys/param.sfo').write_bytes(synthetic_sfo(fields))
    return root


def one_patch_file(app_ver, ids=('CUSA02320',)):
    listed = ''.join(f'<ID>{i}</ID>' for i in ids)
    return f"""<?xml version="1.0"?>
<Patch>
  <TitleID>{listed}</TitleID>
  <Metadata Title="T" Name="P" Author="x" PatchVer="1.0" AppVer="{app_ver}" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00401000" Value="90"/></PatchList>
  </Metadata>
</Patch>"""


class ExecutableVersionTests(unittest.TestCase):
    """The build a patch is for is param.sfo's APP_VER; VERSION is the content base version."""

    def found(self, fields, app_ver):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dump = dump_with(root / 'dump', fields)
            folder = root / 'patches'
            folder.mkdir()
            (folder / 'p.xml').write_text(one_patch_file(app_ver))
            return external_patches(folder, patches.game_versions(dump), app_elf='eboot.bin')

    def test_app_ver_is_what_a_patch_must_name(self):
        fields = {'APP_VER': '01.02', 'VERSION': '01.00'}
        self.assertEqual(self.found(fields, '01.00'), [])  # the content base version: not the build
        self.assertEqual(len(self.found(fields, '01.02')), 1)

    def test_version_is_the_fallback_when_app_ver_is_missing(self):
        self.assertEqual(len(self.found({'VERSION': '01.02'}, '01.02')), 1)
        self.assertEqual(self.found({'VERSION': '01.02'}, '01.00'), [])


class PatchGameTests(unittest.TestCase):
    def folder_with(self, directory, ids):
        folder = Path(directory)
        (folder / 'p.xml').write_text(one_patch_file('01.09', ids))
        return folder

    def test_collection_title_ids_are_not_offered_to_bloodborne(self):
        with tempfile.TemporaryDirectory() as directory:
            ndc_only = self.folder_with(directory, ('CUSA02320', 'CUSA02343'))
            self.assertEqual(external_patches(ndc_only, {'01.09'}, game='bloodborne'), [])
            self.assertEqual(len(external_patches(ndc_only, {'01.09'}, game='ndc')), 1)
        with tempfile.TemporaryDirectory() as directory:
            bb_only = self.folder_with(directory, ('CUSA03173',))
            self.assertEqual(len(external_patches(bb_only, {'01.09'}, game='bloodborne')), 1)
            self.assertEqual(external_patches(bb_only, {'01.09'}, game='ndc'), [])

    def test_unreadable_title_id_fails_closed_on_the_collection_path(self):
        with tempfile.TemporaryDirectory() as directory:
            listed = self.folder_with(directory, ('CUSA02320',))
            self.assertEqual(external_patches(listed, {'01.09'}, fail_closed=True), [])
            self.assertEqual(len(external_patches(listed, {'01.09'}, title_id='CUSA02320',
                                                  fail_closed=True)), 1)
            (listed / 'p.xml').write_text(one_patch_file('01.09', ()))  # lists no title IDs
            self.assertEqual(len(external_patches(listed, {'01.09'}, fail_closed=True)), 1)

    def test_the_collection_path_asks_for_the_title_filter_to_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            out = root / 'out'
            out.mkdir()
            (out / 'eboot.elf').write_bytes(b'')
            argv = ['patches.py', '--external-only', '--patches-dir', str(root),
                    '--game-dir', str(root / 'no-dump'), '--out', str(out)]
            with patch.object(sys, 'argv', argv), \
                 patch.object(patches, 'eboot_segments', return_value=SEGMENTS), \
                 patch.object(patches, 'external_patches', return_value=[]) as called:
                patches.main()
            self.assertTrue(called.call_args.kwargs.get('fail_closed'))


if __name__ == '__main__':
    unittest.main()
