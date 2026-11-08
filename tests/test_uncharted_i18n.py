import ast
import sys
from string import Formatter
import unittest
from unittest.mock import patch

from paths import ROOT

sys.path.insert(0, str(ROOT / 'launcher'))
import uncharted_i18n


class LauncherTranslationTests(unittest.TestCase):
    def test_each_language_translates_play(self):
        expected = {
            'es': 'Jugar', 'pt': 'Jogar', 'fr': 'Jouer', 'de': 'Spielen',
            'it': 'Gioca', 'ja': 'プレイ', 'ru': 'Играть',
        }
        for language, translation in expected.items():
            with self.subTest(language=language):
                self.assertEqual(uncharted_i18n.translate(language, 'Play'), translation)

    def test_missing_text_falls_back_to_english(self):
        self.assertEqual(uncharted_i18n.translate('es', 'Untranslated text'), 'Untranslated text')
        self.assertEqual(uncharted_i18n.translate('en', 'Play'), 'Play')

    def test_automatic_language_uses_windows_locale(self):
        for language in ('es', 'pt', 'fr', 'de', 'it', 'ja', 'ru'):
            with self.subTest(language=language), patch.object(
                    uncharted_i18n.locale, 'getlocale', return_value=(f'{language}_XX', 'UTF-8')):
                self.assertEqual(uncharted_i18n.language_code(''), language)
        with patch.object(uncharted_i18n.locale, 'getlocale', return_value=('ko_KR', 'UTF-8')):
            self.assertEqual(uncharted_i18n.language_code(''), 'en')

    def test_launcher_offers_selected_languages(self):
        self.assertEqual(
            [code for code, _name in uncharted_i18n.LANGUAGES],
            ['', 'en', 'es', 'pt', 'fr', 'ru', 'de', 'it', 'ja'],
        )

    def test_interface_language_help_is_translated(self):
        text = 'This changes only the launcher. The game language is selected on the Game page.'
        self.assertNotEqual(uncharted_i18n.translate('es', text), text)

    def test_each_translation_covers_every_launcher_text(self):
        tree = ast.parse((ROOT / 'launcher' / 'uncharted_launcher.py').read_text(encoding='utf-8'))
        texts = {node.args[0].value for node in ast.walk(tree)
                 if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
                 and node.func.id == '_' and node.args
                 and isinstance(node.args[0], ast.Constant) and isinstance(node.args[0].value, str)}
        for language in ('es', 'pt', 'fr', 'de', 'it', 'ja', 'ru'):
            with self.subTest(language=language):
                self.assertEqual(texts - set(uncharted_i18n.TRANSLATIONS[language]), set())


class RussianFrenchTranslationTests(unittest.TestCase):
    def test_windows_locale_names(self):
        for locale_name, expected in (
                ('Russian_Russia', 'ru'), ('ru_RU', 'ru'),
                ('French_France', 'fr'), ('French_Canada', 'fr'), ('fr_CA', 'fr'),
                (None, 'en')):
            with self.subTest(locale=locale_name), patch.object(
                    uncharted_i18n.locale, 'getlocale', return_value=(locale_name, None)):
                self.assertEqual(uncharted_i18n.language_code(), expected)

    def test_explicit_language_overrides_system_locale(self):
        with patch.object(uncharted_i18n.locale, 'getlocale', return_value=('en_US', None)):
            for language in ('ru', 'fr'):
                self.assertEqual(uncharted_i18n.language_code(language), language)

    def test_missing_text_keeps_english_fallback(self):
        for language in ('ru', 'fr'):
            self.assertEqual(uncharted_i18n.translate(language, 'New untranslated message'),
                             'New untranslated message')

    def test_base_shared_and_extra_translations_are_preserved(self):
        for language, expected in {
                'fr': ('Jouer', 'Ajouter à Steam', 'Vibrations de la manette'),
                'ru': ('Играть', 'Добавить в Steam', 'Вибрация геймпада'),
        }.items():
            with self.subTest(language=language):
                self.assertEqual(tuple(uncharted_i18n.translate(language, text) for text in
                                       ('Play', 'Add to Steam', 'Controller vibration')), expected)

    def test_translations_preserve_format_fields_and_whitespace(self):
        formatter = Formatter()
        for language in ('ru', 'fr'):
            for source, translated in uncharted_i18n.TRANSLATIONS[language].items():
                with self.subTest(language=language, text=source):
                    self.assertTrue(translated.strip())
                    fields = lambda text: [(name, spec, conversion)
                                           for _, name, spec, conversion in formatter.parse(text)
                                           if name is not None]
                    self.assertEqual(fields(source), fields(translated))
                    self.assertEqual(source.count('\n'), translated.count('\n'))
                    self.assertEqual(source.endswith(' '), translated.endswith(' '))
                    self.assertNotIn('\ufffd', translated)

    def test_launcher_labels_and_dynamic_options_are_covered(self):
        tree = ast.parse((ROOT / 'launcher' / 'uncharted_launcher.py').read_text(encoding='utf-8'))
        texts = {node.args[0].value for node in ast.walk(tree)
                 if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
                 and node.func.id == '_' and node.args
                 and isinstance(node.args[0], ast.Constant) and isinstance(node.args[0].value, str)}
        option_tables = {
            'LANGUAGES', 'PRESENT_MODES', 'FRAME_RATES', 'RENDER_HEIGHTS', 'ASPECTS',
            'SPEAKERS', 'SCALERS', 'SHARPENING', 'ANTI_ALIASING', 'AA_SHARPNESS',
            'FRAME_GENERATION', 'BUTTON_PROMPTS', 'FRAME_GEN_MULTIPLIERS',
            'BUTTON_ACTIONS', 'STICK_ACTIONS', 'PAD_BUTTONS', 'MOUSE_SENSITIVITY', 'FIX_CHOICES',
        }
        for node in tree.body:
            if not isinstance(node, ast.Assign) or not isinstance(node.targets[0], ast.Name):
                continue
            name = node.targets[0].id
            if name in option_tables:
                texts.update(row[1] for row in ast.literal_eval(node.value))
            elif name == 'FIXES':
                texts.update(text for row in ast.literal_eval(node.value).values() for text in row[:2])
            elif name == 'RESERVED_KEYS':
                texts.update(ast.literal_eval(node.value).values())
            elif name in ('FSR411', 'XESS'):
                texts.add(ast.literal_eval(node.value)[1])
        texts.update(('Bronze', 'Silver', 'Gold', 'Platinum', 'Automatic (Windows)'))
        # Physical labels, algorithm names and numeric sizes are language-independent.
        invariant = {'L1', 'R1', 'L3', 'R3', 'LB / L1', 'RB / R1', 'LT / L2', 'RT / R2',
                     'TAA', '1440p', '1620p', '2160p (4K)'}
        texts = {text for text in texts - invariant if not text.replace('.', '').isdigit()}
        for language in ('ru', 'fr'):
            with self.subTest(language=language):
                self.assertEqual(texts - uncharted_i18n.TRANSLATIONS[language].keys(), set())


if __name__ == '__main__':
    unittest.main()
