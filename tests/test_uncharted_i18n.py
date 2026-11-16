import ast
import sys
import unittest
from unittest.mock import patch

from paths import ROOT

sys.path.insert(0, str(ROOT / 'launcher'))
import uncharted_i18n


class LauncherTranslationTests(unittest.TestCase):
    def test_each_language_translates_play(self):
        expected = {
            'es': 'Jugar', 'pt': 'Jogar', 'fr': 'Jouer', 'de': 'Spielen',
            'it': 'Gioca', 'ja': 'プレイ',
        }
        for language, translation in expected.items():
            with self.subTest(language=language):
                self.assertEqual(uncharted_i18n.translate(language, 'Play'), translation)

    def test_missing_text_falls_back_to_english(self):
        self.assertEqual(uncharted_i18n.translate('es', 'Untranslated text'), 'Untranslated text')
        self.assertEqual(uncharted_i18n.translate('en', 'Play'), 'Play')

    def test_automatic_language_uses_windows_locale(self):
        for language in ('es', 'pt', 'fr', 'de', 'it', 'ja'):
            with self.subTest(language=language), patch.object(
                    uncharted_i18n.locale, 'getlocale', return_value=(f'{language}_XX', 'UTF-8')):
                self.assertEqual(uncharted_i18n.language_code(''), language)
        with patch.object(uncharted_i18n.locale, 'getlocale', return_value=('ko_KR', 'UTF-8')):
            self.assertEqual(uncharted_i18n.language_code(''), 'en')

    def test_launcher_offers_selected_languages(self):
        self.assertEqual(
            [code for code, _name in uncharted_i18n.LANGUAGES],
            ['', 'en', 'es', 'pt', 'fr', 'de', 'it', 'ja'],
        )

    def test_interface_language_help_is_translated(self):
        text = 'This changes only the launcher. The game language is selected on the Game page.'
        self.assertNotEqual(uncharted_i18n.translate('es', text), text)

    def test_controller_editor_labels_and_placeholders_are_localized(self):
        texts=('Saved profiles','Create new profile','Load profile','Save changes',
               'Your controller button','Game button (PS4)','Show all assignments',
               'Face buttons','Layout and assignments','In-game icons','Test buttons',
               'Profile loaded: {}. Changes apply on the next game start.',
               'Delete profile "{}"? Current settings will be kept.',
               'Unapplied changes: {}. Select each game button to apply.')
        for language in ('es','pt','fr','de','it','ja'):
            for text in texts:
                with self.subTest(language=language,text=text):
                    translated=uncharted_i18n.translate(language,text)
                    self.assertNotEqual(translated,text)
                    self.assertEqual(translated.count('{}'),text.count('{}'))

    def test_each_translation_covers_every_launcher_text(self):
        tree = ast.parse((ROOT / 'launcher' / 'uncharted_launcher.py').read_text(encoding='utf-8'))
        texts = {node.args[0].value for node in ast.walk(tree)
                 if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
                 and node.func.id == '_' and node.args
                 and isinstance(node.args[0], ast.Constant) and isinstance(node.args[0].value, str)}
        for language in ('es', 'pt', 'fr', 'de', 'it', 'ja'):
            with self.subTest(language=language):
                self.assertEqual(texts - set(uncharted_i18n.TRANSLATIONS[language]), set())


if __name__ == '__main__':
    unittest.main()
