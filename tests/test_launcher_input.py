"""Launcher input.ini generation tests."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from paths import ROOT


SPEC = importlib.util.spec_from_file_location(
    'uncharted_launcher', ROOT / 'launcher' / 'uncharted_launcher.py')
LAUNCHER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LAUNCHER)


class InputConfigTests(unittest.TestCase):
    def test_shortcut_preserves_legacy_custom_controller_bindings(self):
        saved={'pad_bindings':{'cross':'y|paddle2'}}
        with mock.patch.object(LAUNCHER,'load_json',return_value=saved), \
             mock.patch.object(LAUNCHER.sys,'argv',['Uncharted.exe','--play']), \
             mock.patch.object(LAUNCHER,'game_available',return_value=True), \
             mock.patch.object(LAUNCHER,'play_without_window',return_value=0) as play:
            with self.assertRaises(SystemExit):
                LAUNCHER.main()
        self.assertEqual(LAUNCHER.effective_pad_bindings(play.call_args.args[0])['cross'],'y|paddle2')
        with mock.patch.object(LAUNCHER,'load_json',return_value={**saved,'pad_mapping_mode':'recommended'}):
            self.assertEqual(LAUNCHER.effective_pad_bindings(LAUNCHER.load_settings())['cross'],'a')

    def test_controller_profile_round_trip_and_scope(self):
        settings = {'controller_id':'device-a','game_dir':'keep-game','key_bindings':{'cross':'Z'},
                    'controller_model':'steam_deck','pad_mapping_mode':'recommended',
                    'pad_bindings':{'cross':'a|paddle2'},'pad_vibration':False,
                    'pad_vibration_strength':'35','pad_invert_y':True,'button_prompts':'nintendo'}
        profile = LAUNCHER.controller_preset(settings)
        for excluded in ('controller_id','game_dir','key_bindings'):
            self.assertNotIn(excluded,profile)
        self.assertEqual(profile['pad_bindings']['cross'],'a|paddle2')
        self.assertFalse(profile['pad_vibration'])
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(LAUNCHER,'CONFIG_DIR',Path(directory)):
            self.assertEqual(LAUNCHER.read_controller_presets(),{})
            LAUNCHER.write_controller_presets({'José':profile})
            settings['pad_bindings']['cross']='b'
            restored=LAUNCHER.read_controller_presets()['José']
            self.assertEqual(restored,profile)
            self.assertEqual(LAUNCHER.controller_preset(restored),profile)
        for invalid in ({'controller_model':[]},{'pad_vibration':'false'},
                        {'pad_bindings':{'cross':'a\npad.circle=b'}},{'pad_bindings':[]}):
            with self.assertRaises(ValueError):
                LAUNCHER.controller_preset(invalid)

    def test_controller_profile_write_failure_keeps_file_and_cleans_temporary(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(LAUNCHER,'CONFIG_DIR',Path(directory)):
            LAUNCHER.write_controller_presets({'Original':LAUNCHER.controller_preset({})})
            path=Path(directory)/'controller_profiles.json'
            before=path.read_bytes()
            with mock.patch.object(LAUNCHER.os,'replace',side_effect=PermissionError('locked')):
                with self.assertRaises(PermissionError):
                    LAUNCHER.write_controller_presets({'Other':LAUNCHER.controller_preset({})})
            self.assertEqual(path.read_bytes(),before)
            self.assertEqual(list(Path(directory).glob('*.tmp')),[])
            path.write_text('{broken',encoding='utf-8')
            with self.assertRaises(ValueError):
                LAUNCHER.read_controller_presets()

    def test_two_bindings_round_trip_and_legacy_defaults(self):
        self.assertEqual(LAUNCHER.effective_pad_bindings({'pad_bindings':{'cross':'b'}})['cross'],'b')
        self.assertEqual(LAUNCHER.split_pad_binding('a'), ('a','none'))
        self.assertEqual(LAUNCHER.split_pad_binding('a|paddle2'), ('a','paddle2'))
        self.assertEqual(LAUNCHER.split_pad_binding('none|paddle2'), ('none','paddle2'))
        self.assertEqual(LAUNCHER.split_pad_binding('a\npad.circle=b'), ('none','none'))
        settings = {'pad_mapping_mode':'custom','pad_bindings':{'cross':'a|paddle2'}}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'input.ini'
            with mock.patch.object(LAUNCHER,'input_ini_path',return_value=path):
                LAUNCHER.write_input_ini(settings)
                self.assertIn('pad.cross=a|paddle2\n',path.read_text())
                settings['pad_mapping_mode']='recommended'
                LAUNCHER.write_input_ini(settings)
                self.assertIn('pad.cross=a\n',path.read_text())
                self.assertEqual(settings['pad_bindings']['cross'],'a|paddle2')

    def test_unknown_capabilities_differ_from_no_buttons(self):
        self.assertIsNone(LAUNCHER.available_pad_buttons(None))
        self.assertIsNone(LAUNCHER.available_pad_buttons({'type':'ps5'}))
        self.assertEqual(LAUNCHER.available_pad_buttons({'buttons':[],'axes':[]}),set())
        info = LAUNCHER.parse_controller_probe('connected=1\ncount=1\ncontroller.0.id=a\n'
            'controller.0.buttons=a,b,paddle2\ncontroller.0.axes=leftx,lefttrigger\n')
        self.assertEqual(LAUNCHER.available_pad_buttons(info['controllers'][0]),
                         {'a','b','paddle2','lefttrigger'})

    def test_model_override_never_invents_capabilities(self):
        pad = {'type':'steam','buttons':['a'],'axes':[]}
        self.assertEqual(LAUNCHER.effective_controller_model({},pad),'generic')
        self.assertEqual(LAUNCHER.effective_controller_model({'controller_model':'steam_deck'},pad),'steam_deck')
        self.assertEqual(LAUNCHER.available_pad_buttons(pad),{'a'})
        for kind,model in [('ps3','ps3'),('ps4','playstation'),('ps5','ps5'),('switchpro','nintendo'),
                           ('gamecube','gamecube')]:
            self.assertEqual(LAUNCHER.effective_controller_model({}, {'type':kind}),model)

    def test_gamecube_labels_presets_and_prompt_override(self):
        labels=dict(LAUNCHER.pad_button_options('gamecube'))
        self.assertEqual([labels[k] for k in ('a','b','x','y')],['A','X','B','Y'])
        self.assertEqual([labels[k] for k in ('lefttrigger','righttrigger','rightshoulder')],['L','R','Z'])
        settings={'controller_profile':'gamecube','controller_model':'gamecube','button_prompts':'gamecube',
                  'pad_mapping_mode':'custom','pad_bindings':{'l1':'misc3','touchpad':'start'}}
        self.assertEqual(LAUNCHER.controller_preset(settings)['controller_model'],'gamecube')
        self.assertEqual(LAUNCHER.controller_preset(settings)['button_prompts'],'gamecube')
        pad={'type':'gamecube','profile':'gamecube','buttons':['a','b','x','y','start','rightshoulder'],
             'axes':['lefttrigger','righttrigger']}
        self.assertNotIn('leftstick',LAUNCHER.available_pad_buttons(pad))
        self.assertNotIn('back',LAUNCHER.available_pad_buttons(pad))
        self.assertEqual(LAUNCHER.effective_controller_profile({},pad),'gamecube')

    def test_controller_vibration_defaults_on_and_can_be_disabled(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'input.ini'
            original = LAUNCHER.input_ini_path
            LAUNCHER.input_ini_path = lambda: path
            try:
                LAUNCHER.write_input_ini({})
                text = path.read_text(encoding='utf-8')
                self.assertIn('pad_vibration=1\n', text)
                self.assertIn('pad_vibration_strength=100\n', text)

                LAUNCHER.write_input_ini({'pad_vibration': False, 'pad_vibration_strength': '45'})
                text = path.read_text(encoding='utf-8')
                self.assertIn('pad_vibration=0\n', text)
                self.assertIn('pad_vibration_strength=45\n', text)
            finally:
                LAUNCHER.input_ini_path = original

    def test_vibration_strength_is_clamped(self):
        self.assertEqual(LAUNCHER.vibration_strength({'pad_vibration_strength': '-5'}), 0)
        self.assertEqual(LAUNCHER.vibration_strength({'pad_vibration_strength': '250'}), 100)
        self.assertEqual(LAUNCHER.vibration_strength({'pad_vibration_strength': 'invalid'}), 100)
        self.assertEqual(LAUNCHER.controller_preset({'pad_vibration_strength': 'inf'})['pad_vibration_strength'], '100')

    def test_controller_probe_response(self):
        info = LAUNCHER.parse_controller_probe(
            'connected=1\ncount=1\ncontroller.0.id=abc123\n'
            'controller.0.name=DualSense Wireless Controller\ncontroller.0.type=ps5\n'
            'controller.0.profile=playstation\ncontroller.0.south=Cross\n'
            'controller.0.east=Circle\ncontroller.0.west=Square\n'
            'controller.0.north=Triangle\ncontroller.0.rumble=1\n'
            'controller.0.touchpad=1\nrumble=1\ntested=1\n')
        self.assertTrue(info['connected'])
        self.assertTrue(info['tested'])
        self.assertEqual(info['controllers'][0], {
            'id': 'abc123', 'name': 'DualSense Wireless Controller', 'type': 'ps5',
            'profile': 'playstation',
            'labels': {'south': 'Cross', 'east': 'Circle', 'west': 'Square', 'north': 'Triangle'},
            'rumble': True, 'touchpad': True,
        })

    def test_recommended_mapping_does_not_destroy_custom_bindings(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'input.ini'
            original = LAUNCHER.input_ini_path
            LAUNCHER.input_ini_path = lambda: path
            try:
                settings = {'pad_mapping_mode': 'recommended', 'pad_bindings': {'cross': 'b'}}
                LAUNCHER.write_input_ini(settings)
                self.assertIn('pad.cross=a\n', path.read_text(encoding='utf-8'))
                self.assertEqual(settings['pad_bindings'], {'cross': 'b'})
                settings['pad_mapping_mode'] = 'custom'
                LAUNCHER.write_input_ini(settings)
                self.assertIn('pad.cross=b\n', path.read_text(encoding='utf-8'))
            finally:
                LAUNCHER.input_ini_path = original

    def test_controller_profile_override_and_labels(self):
        controller = {'profile': 'xbox'}
        self.assertEqual(LAUNCHER.effective_controller_profile({}, controller), 'xbox')
        self.assertEqual(LAUNCHER.effective_controller_profile(
            {'controller_profile': 'nintendo'}, controller), 'nintendo')
        labels = dict(LAUNCHER.pad_button_options('nintendo'))
        self.assertEqual((labels['a'], labels['b'], labels['x'], labels['y']), ('B', 'A', 'Y', 'X'))
        self.assertEqual(labels['misc1'], 'Capture')
        self.assertEqual(dict(LAUNCHER.pad_button_options('steam'))['misc1'], 'Quick access')

    def test_missing_selected_controller_is_not_replaced_in_the_ui(self):
        window = LAUNCHER.Launcher.__new__(LAUNCHER.Launcher)
        window.controllers = [{'id': 'first'}, {'id': 'second'}]
        selected = mock.Mock()
        window.var = lambda _key: selected
        selected.get.return_value = 'second'
        self.assertEqual(window.selected_controller(), {'id': 'second'})
        selected.get.return_value = 'missing'
        self.assertIsNone(window.selected_controller())
        selected.get.return_value = 'auto'
        self.assertEqual(window.selected_controller(), {'id': 'first'})

    def test_selected_controller_and_profile_are_exported_to_runtime(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'input.ini'
            settings = {**LAUNCHER.APP_DEFAULTS, 'game_dir': directory,
                        'controller_id': '0123456789abcdef',
                        'controller_profile': 'nintendo'}
            with mock.patch.dict(os.environ, {'BB_INPUT_CONFIG': str(path)}):
                env = LAUNCHER.game_environment(settings)
            self.assertEqual(env['BB_PAD_DEVICE'], '0123456789abcdef')
            self.assertEqual(env['BB_PAD_PROFILE'], 'nintendo')
            self.assertEqual(env['BB_INPUT_CONFIG'], str(path))


if __name__ == '__main__':
    unittest.main()
