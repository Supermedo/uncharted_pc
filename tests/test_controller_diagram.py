"""Windows-friendly, hidden Tk checks; never open or drive the desktop UI."""
import sys
import unittest
import subprocess
import tempfile
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'launcher'))
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
import uncharted_launcher as launcher
from uncharted_controller_art import MODELS, controller_art, controller_rear_art
import uncharted_widgets
from uncharted_widgets import ControllerDiagram


class ControllerDiagramTests(unittest.TestCase):
    def setUp(self):
        self.root = tk.Tk()
        self.root.withdraw()

    def tearDown(self):
        self.root.update_idletasks()
        for job in self.root.tk.call('after','info'):
            self.root.tk.call('after','cancel',job)
        uncharted_widgets._cache.clear()
        self.root.destroy()

    def test_models_click_targets_dpi_and_bounded_cache(self):
        for dpi in (1,1.25,1.5,2):
            px = lambda n: int(n*dpi)
            clicked = []
            diagram = ControllerDiagram(tk,self.root,launcher.COLORS,px,('Segoe UI',10),on_select=clicked.append)
            for model,_label in MODELS[1:]:
                diagram.set_controller(model,model,True,{'cross':'a|paddle2'})
                x,y = diagram.targets['a']
                diagram.pick(SimpleNamespace(x=round(x*dpi),y=round(y*dpi)))
                self.assertEqual(clicked[-1],'a')
                self.assertIn('lefttrigger',diagram.buttons)
                self.assertEqual(diagram.assignments('paddle2'),['cross'])
                self.assertLessEqual(len(diagram.images),2)
                if model.startswith('steam'):
                    self.assertIn('paddle4',diagram.buttons)
                    self.assertLess(diagram.targets['paddle1'][0],diagram.targets['paddle2'][0])
                for width in (540,320,400):
                    diagram.resize(SimpleNamespace(width=px(width)))
                    diagram.canvas.after_cancel(diagram.resize_job)
                    diagram.finish_resize()
                    self.assertAlmostEqual(diagram.width,width,delta=1)
                    self.assertEqual(bool(diagram.callouts),width>=500)
                    for key,(x,y) in diagram.targets.items():
                        self.assertTrue(0<=x<=width,(model,key,x,width))
                        self.assertEqual(diagram.hit_test(SimpleNamespace(x=x*dpi,y=y*dpi)),key,(model,key))
                diagram.resize(SimpleNamespace(width=px(680)))
                diagram.canvas.after_cancel(diagram.resize_job)
                diagram.finish_resize()
            diagram.frame.destroy()

    def test_launcher_preserves_custom_map_and_rejects_unavailable_buttons(self):
        with mock.patch.object(launcher,'load_json',return_value={'check_updates':False,'pad_bindings':{'circle':'b'}}):
            ui = launcher.Launcher(self.root,tk,ttk,filedialog,messagebox)
        self.assertEqual(ui.var('pad_mapping_mode').get(),'custom')
        ui.var('pad_mapping_mode').set('custom')
        ui.var('controller_model').set('steam_deck')
        ui.update_controller_ui()
        ui.controller_diagram.select('paddle2')
        ui.stage_selected_control()
        ui.apply_mapping()
        self.assertEqual(ui.pad_vars['cross'].get(),'a|paddle2')

        ui.var('pad_mapping_mode').set('recommended')
        ui.update_mapping_controls()
        ui.apply_mapping()
        self.assertEqual(ui.pad_vars['cross'].get(),'a|paddle2')
        self.assertEqual(ui.controller_diagram.bindings['cross'],'a')
        self.assertEqual(str(ui.binding_boxes[0]['state']),'disabled')
        ui.controllers = [{'id':'test','name':'Virtual','type':'xboxone','profile':'xbox',
                           'buttons':['a','b'],'axes':[],'rumble':False,'touchpad':False}]
        ui.var('controller_id').set('test')
        ui.var('pad_mapping_mode').set('custom')
        ui.var('controller_model').set('steam_deck')
        ui.update_controller_ui()
        ui.controller_diagram.select('paddle4')
        ui.stage_selected_control()
        ui.apply_mapping()
        self.assertEqual(ui.pad_vars['circle'].get(),'b')
        self.assertNotIn('paddle4',ui.binding_boxes[1]._pad_values)
        self.assertEqual(ui.pad_vars['cross'].get(),'a|paddle2')
        ui.var('controller_model').set('ps5')
        ui.update_controller_ui()
        self.assertEqual(ui.pad_vars['cross'].get(),'a|paddle2')

    def test_assignment_table_selection_resize_and_capabilities(self):
        selected=[]
        diagram=ControllerDiagram(tk,self.root,launcher.COLORS,int,('Segoe UI',10),on_select=selected.append)
        diagram.set_controller('gamecube','',False,{'cross':'a','circle':'a|b'},
                               {'a':'A','b':'X'}, {'a','b'}, {'cross':'Cross ✕','circle':'Circle ○'})
        diagram.show_all.set(True)
        diagram.toggle_assignments()
        entry,arrow,target=diagram.assignment_rows['a']
        self.assertEqual(target['text'],'Cross ✕ / Circle ○')
        self.assertIn('Not exposed',diagram.assignment_rows['leftshoulder'][2]['text'])
        callbacks=tuple(target._tclCommands)
        for _ in range(1000):
            # Invoke the target-label handler without displaying the window.
            command=target._tclCommands[-1]
            self.root.tk.call(command,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,str(target),0,0,0)
        self.assertEqual(selected,['a']*1000)
        self.assertEqual(tuple(target._tclCommands),callbacks)
        self.assertEqual(entry['highlightbackground'],launcher.COLORS['gold'])
        diagram.buttons['b'].invoke()
        self.assertEqual(selected[-1],'b')
        self.assertEqual(entry['highlightbackground'],launcher.COLORS['line'])
        for width,columns in ((1000,2),(400,1),(1000,2)):
            diagram.width=width
            diagram.draw()
            visible=[group for group,keys in diagram.group_frames if group.grid_info()]
            self.assertEqual({int(group.grid_info()['column']) for group in visible},set(range(columns)))
            self.assertIs(diagram.assignment_rows['a'][0],entry)
        diagram.show_all.set(False)
        diagram.toggle_assignments()
        self.assertFalse(diagram.button_frame.grid_info())

    def test_gamecube_has_physical_controls_without_invented_stick_clicks(self):
        _,controls=controller_art('gamecube')
        self.assertTrue({'a','b','x','y','lefttrigger','righttrigger','rightshoulder','start'} <= controls.keys())
        self.assertTrue({'back','leftstick','rightstick','touchpad'}.isdisjoint(controls))
        self.assertIn('leftshoulder',controls)
        self.assertGreater(controls['leftshoulder'][1],controls['lefttrigger'][1])
        self.assertGreater(controls['a'][2],controls['x'][2])  # Large A, small B.
        self.assertLess(controls['x'][0],controls['a'][0])
        self.assertGreater(controls['b'][0],controls['a'][0])

    def test_gamecube_optional_shoulder_requires_exposed_input(self):
        ui=self.make_launcher()
        pad={'id':'gc','name':'GameCube','type':'gamecube','profile':'gamecube',
             'buttons':['a','b','x','y','start','rightshoulder'],'axes':['lefttrigger','righttrigger'],
             'rumble':False,'touchpad':False}
        ui.controllers=[pad]
        ui.var('controller_id').set('gc')
        ui.var('controller_model').set('gamecube')
        ui.var('pad_mapping_mode').set('custom')
        ui.update_controller_ui()
        ui.controller_diagram.select('leftshoulder')
        self.assertEqual(str(ui.mapping_use_selected['state']),'disabled')
        self.assertNotIn('leftshoulder',ui.binding_boxes[1]._pad_values)
        pad['buttons'].append('leftshoulder')
        ui.update_controller_ui()
        ui.controller_diagram.select('leftshoulder')
        self.assertEqual(str(ui.mapping_use_selected['state']),'normal')
        ui.stage_selected_control()
        ui.apply_mapping()
        self.assertIn('leftshoulder',launcher.split_pad_binding(ui.pad_vars['l1'].get()))

    def test_thousand_clicks_keep_callbacks_widgets_images_and_focus(self):
        selected=[]
        diagram=ControllerDiagram(tk,self.root,launcher.COLORS,int,('Segoe UI',10),on_select=selected.append)
        diagram.set_controller('steam_deck','Deck',True)
        canvas=diagram.canvas
        callbacks=tuple(canvas._tclCommands)
        images=tuple(self.root.tk.call('image','names'))
        widgets={k:str(v) for k,v in diagram.buttons.items()}
        items=canvas.find_all()
        focus=self.root.focus_get()
        view=canvas.yview()
        keys=list(diagram.hit_regions)
        for i in range(1000):
            x,y=diagram.targets[keys[i%len(keys)]]
            diagram.pick(SimpleNamespace(x=x,y=y))
        self.assertEqual(len(selected),1000)
        self.assertEqual(tuple(canvas._tclCommands),callbacks)
        self.assertEqual(tuple(self.root.tk.call('image','names')),images)
        self.assertEqual(canvas.find_all(),items)
        self.assertEqual({k:str(v) for k,v in diagram.buttons.items()},widgets)
        self.assertEqual(self.root.focus_get(),focus)
        self.assertEqual(canvas.yview(),view)
        diagram.keypress(SimpleNamespace(keysym='Home'))
        self.assertEqual(selected[-1],keys[0])
        diagram.frame.destroy()

    def test_resize_events_coalesce_and_callbacks_stay_bounded(self):
        errors=[]
        self.root.report_callback_exception=lambda *error:errors.append(error)
        diagram=ControllerDiagram(tk,self.root,launcher.COLORS,int,('Segoe UI',10))
        initial=len(diagram.canvas._tclCommands)
        for i in range(100):
            diagram.resize(SimpleNamespace(width=580+i))
        self.assertEqual(len(diagram.canvas._tclCommands),initial+1)
        self.root.tk.call('after',50)
        self.root.update()
        self.assertAlmostEqual(diagram.width,679,delta=2)
        self.assertEqual(len(diagram.canvas._tclCommands),initial)
        for model,_label in MODELS[1:]:
            diagram.set_controller(model,model,True)
            self.root.update_idletasks()
            self.assertEqual(len(diagram.canvas._tclCommands),initial)
            self.assertLessEqual(len(diagram.images),2)
        diagram.resize(SimpleNamespace(width=600))
        diagram.frame.destroy()
        self.root.tk.call('after',50)
        self.root.update()
        self.assertEqual(errors,[])

    def make_launcher(self):
        with mock.patch.object(launcher,'load_json',return_value={'check_updates':False}):
            return launcher.Launcher(self.root,tk,ttk,filedialog,messagebox)

    def test_saved_profiles_restore_without_changing_device_or_other_settings(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(launcher,'CONFIG_DIR',Path(directory)):
            ui=self.make_launcher()
            ui.var('controller_id').set('keep-device')
            ui.var('game_dir').set('keep-game')
            ui.var('controller_model').set('steam_deck')
            ui.var('pad_mapping_mode').set('custom')
            ui.var('button_prompts').set('xbox')
            ui.var('pad_vibration').set(False)
            ui.var('pad_vibration_strength').set('35')
            ui.pad_vars['cross'].set('a|paddle2')
            ui.save_controller_preset(create=True)
            self.assertEqual(ui.controller_preset_name.get(),'Profile 1')
            ui.var('controller_model').set('xbox')
            ui.var('pad_vibration').set(True)
            ui.var('pad_vibration_strength').set('100')
            ui.pad_vars['cross'].set('b')
            ui.save_controller_preset(create=True)
            self.assertEqual(ui.controller_preset_name.get(),'Profile 2')
            ui.controller_preset_name.set('Profile 1')
            self.assertEqual(ui.pad_vars['cross'].get(),'b')
            ui.load_controller_preset()
            self.assertEqual(ui.pad_vars['cross'].get(),'a|paddle2')
            self.assertEqual(ui.var('controller_model').get(),'steam_deck')
            self.assertEqual(ui.var('controller_id').get(),'keep-device')
            self.assertEqual(ui.var('game_dir').get(),'keep-game')
            self.assertFalse(ui.var('pad_vibration').get())
            self.assertEqual(ui.var('pad_vibration_strength').get(),'35')
            self.assertEqual(ui.var('button_prompts').get(),'xbox')
            self.assertIn('paddle2',ui.controller_diagram.visible_controls)
            self.assertEqual(len(launcher.read_controller_presets()),2)
            with mock.patch.object(ui,'controller_dialog',return_value=False):
                ui.pad_vars['cross'].set('y')
                ui.save_controller_preset()
                ui.delete_controller_preset()
            self.assertEqual(launcher.read_controller_presets()['Profile 1']['pad_bindings']['cross'],'a|paddle2')
            with mock.patch.object(ui,'controller_dialog',return_value=True):
                ui.controller_preset_name.set('PROFILE 1')
                ui.save_controller_preset()
                self.assertEqual(ui.controller_preset_name.get(),'Profile 1')
                self.assertEqual(len(launcher.read_controller_presets()),2)
                self.assertEqual(launcher.read_controller_presets()['Profile 1']['pad_bindings']['cross'],'y')
                ui.delete_controller_preset()
            self.assertEqual(list(launcher.read_controller_presets()),['Profile 2'])
            self.assertEqual(ui.pad_vars['cross'].get(),'y')

    def test_profile_failures_leave_settings_and_saved_data_untouched(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(launcher,'CONFIG_DIR',Path(directory)):
            ui=self.make_launcher()
            ui.controller_preset_name.set('   ')
            ui.save_controller_preset()
            self.assertEqual(launcher.read_controller_presets(),{})
            ui.controller_preset_name.set('Test')
            with mock.patch.object(launcher,'write_controller_presets',side_effect=PermissionError('locked')):
                ui.save_controller_preset(create=True)
            self.assertIn('locked',ui.controller_preset_status['text'])
            self.assertEqual(launcher.read_controller_presets(),{})
            path=Path(directory)/'controller_profiles.json'
            path.write_text('{broken',encoding='utf-8')
            previous={a:v.get() for a,v in ui.pad_vars.items()}
            ui.load_controller_preset()
            ui.save_controller_preset()
            self.assertEqual(path.read_text(),'{broken')
            self.assertEqual({a:v.get() for a,v in ui.pad_vars.items()},previous)

    def test_numbered_profiles_preserve_existing_names_and_section_order(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(launcher,'CONFIG_DIR',Path(directory)):
            legacy=launcher.controller_preset({'pad_bindings':{'cross':'y'}})
            launcher.write_controller_presets({'José':legacy})
            sections=[]
            section=launcher.Launcher.section
            def record(ui,parent,title,*args,**kwargs):
                sections.append(title)
                return section(ui,parent,title,*args,**kwargs)
            with mock.patch.object(launcher.Launcher,'section',record):
                ui=self.make_launcher()
            expected=['Saved profiles','Active controller','Vibration','Layout and assignments','In-game icons']
            self.assertEqual([name for name in sections if name in expected],expected)
            self.assertEqual(str(ui.controller_preset_box['state']),'readonly')
            self.assertTrue(all(button.state=='disabled' for button in ui.controller_preset_actions))
            for number in range(1,4):
                ui.save_controller_preset(create=True)
                self.assertEqual(ui.controller_preset_name.get(),f'Profile {number}')
                self.assertTrue(all(button.state=='normal' for button in ui.controller_preset_actions))
            profiles=launcher.read_controller_presets()
            self.assertEqual(set(profiles),{'José','Profile 1','Profile 2','Profile 3'})
            self.assertEqual(profiles['José'],legacy)
            ui.controller_preset_name.set('José')
            ui.load_controller_preset()
            self.assertEqual(ui.pad_vars['cross'].get(),'y')

    def test_inline_editor_stages_then_applies_and_icons_are_independent(self):
        ui=self.make_launcher()
        ui.var('controller_model').set('steam_deck')
        ui.var('pad_mapping_mode').set('custom')
        ui.update_controller_ui()
        ui.controller_diagram.select('paddle2')
        ui.stage_selected_control()
        self.assertEqual(ui.pad_vars['cross'].get(),'a')
        ui.apply_mapping()
        self.assertEqual(ui.pad_vars['cross'].get(),'a|paddle2')
        self.assertEqual(len([w for w in self.root.winfo_children() if isinstance(w,tk.Toplevel)]),0)
        bindings={a:v.get() for a,v in ui.pad_vars.items()}
        for style in ('0','xbox','keyboard','auto'):
            ui.var('button_prompts').set(style)
            ui.update_prompt_preview()
            self.assertEqual({a:v.get() for a,v in ui.pad_vars.items()},bindings)
            self.assertEqual(ui.var('controller_model').get(),'steam_deck')
        ui.var('button_prompts').set('0')
        ui.update_prompt_preview()
        self.assertIn('\u25a2',ui.prompt_preview['text'])
        with mock.patch.object(ui,'update_mapping_editor',wraps=ui.update_mapping_editor) as refresh:
            before=tuple(ui.controller_diagram.canvas._tclCommands)
            for i in range(1000):
                ui.controller_diagram.select('a' if i%2 else 'paddle2')
            self.assertEqual(refresh.call_count,1000)
            self.assertEqual(tuple(ui.controller_diagram.canvas._tclCommands),before)
        ui.mapping_workspace.grid_remove()
        for width in (1100,920,919,700,660):
            ui.mapping_workspace.place(x=0,y=0,width=ui.px(width),height=ui.px(800))
            self.root.update_idletasks()
            diagram=ui.controller_diagram.frame
            editor=ui.mapping_editor
            self.assertEqual(int(editor.grid_info()['row']),0)
            self.assertEqual(int(editor.grid_info()['column']),1)
            self.assertGreaterEqual(editor.winfo_x(),diagram.winfo_x()+diagram.winfo_width())
            self.assertLessEqual(editor.winfo_x()+editor.winfo_width(),ui.px(width))
            self.assertGreaterEqual(diagram.winfo_width(),ui.px(320))

    def test_game_button_selector_highlights_without_changing_bindings(self):
        ui=self.make_launcher()
        diagram=ui.controller_diagram
        def choose(action):
            index=next(i for i,item in enumerate(launcher.BUTTON_ACTIONS) if item[0]==action)
            ui.mapping_action.current(index)
            ui.mapping_action.event_generate('<<ComboboxSelected>>')
            self.assertEqual(ui.mapping_action.current(),index)
        for model,_label in MODELS[1:]:
            ui.var('controller_model').set(model)
            ui.var('pad_mapping_mode').set('recommended')
            ui.update_controller_ui()
            for action,binding in launcher.DEFAULT_PAD.items():
                choose(action)
                expected=next((button for button in launcher.split_pad_binding(binding)
                               if button in diagram.hit_regions),None)
                self.assertEqual(diagram.selected,expected,(model,action))
                self.assertEqual(ui.selected_control,expected)
                if expected:
                    x,y,r=diagram.hit_regions[expected]
                    self.assertEqual(diagram.canvas.coords(diagram.rings['selected']),
                                     [ui.px(v) for v in (x-r-3,y-r-3,x+r+3,y+r+3)])
        ui.var('controller_model').set('steam_deck')
        ui.var('pad_mapping_mode').set('custom')
        ui.pad_vars['cross'].set('b')
        ui.pad_vars['circle'].set('b')
        ui.update_controller_ui()
        choose('circle')  # Shared sources must not change the chosen game action.
        self.assertEqual(diagram.selected,'b')
        ui.pad_vars['cross'].set('none|paddle2')
        choose('cross')
        self.assertEqual(diagram.selected,'paddle2')
        ui.pad_vars['cross'].set('none')
        choose('cross')
        self.assertIsNone(diagram.selected)
        self.assertEqual(diagram.canvas.itemcget(diagram.rings['selected'],'state'),'hidden')
        ui.pad_vars['cross'].set('a|paddle2')
        bindings={a:v.get() for a,v in ui.pad_vars.items()}
        canvas=diagram.canvas
        before=(tuple(canvas._tclCommands),canvas.find_all(),
                tuple(self.root.tk.call('image','names')),self.root.focus_get(),canvas.yview())
        with mock.patch.object(ui,'update_mapping_editor',wraps=ui.update_mapping_editor) as refresh:
            for i in range(1000):
                choose('cross' if i%2 else 'circle')
            self.assertEqual(refresh.call_count,1000)
        self.assertEqual(before,(tuple(canvas._tclCommands),canvas.find_all(),
                         tuple(self.root.tk.call('image','names')),self.root.focus_get(),canvas.yview()))
        self.assertEqual({a:v.get() for a,v in ui.pad_vars.items()},bindings)
        self.assertEqual(canvas.itemcget(diagram.rings['selected'],'state'),'normal')

    def test_detection_errors_and_timeout_restore_controls(self):
        ui=self.make_launcher()
        ui.show_controller(None,'test failure',False)
        self.assertEqual(ui.controller_refresh_button.state,'normal')
        self.assertIn('test failure',ui.controller_status['text'])
        def run_inline(target,**_kw):
            return SimpleNamespace(start=target)
        with mock.patch.object(launcher,'controller_probe_path',return_value=Path('probe.exe')), \
             mock.patch.object(launcher.threading,'Thread',side_effect=run_inline), \
             mock.patch.object(launcher.subprocess,'run',side_effect=subprocess.TimeoutExpired('probe',5)):
            ui.detect_controller()
        ui.ui_calls.get_nowait()()
        self.assertEqual(ui.controller_refresh_button.state,'normal')
        self.assertEqual(ui.controller_test_button.state,'disabled')

    def test_vibration_result_is_not_attributed_to_a_new_selection(self):
        ui=self.make_launcher()
        ui.var('controller_id').set('first')
        def inline(target,**_kw):
            return SimpleNamespace(start=target)
        with mock.patch.object(launcher,'controller_probe_path',return_value=Path('probe.exe')), \
             mock.patch.object(launcher.threading,'Thread',side_effect=inline), \
             mock.patch.object(launcher.subprocess,'run',return_value=SimpleNamespace(
                 returncode=0,stdout='connected=0\ncount=0\ntested=1\n')):
            ui.detect_controller(test=True)
        ui.var('controller_id').set('second')
        with mock.patch.object(ui,'show_controller') as show:
            ui.ui_calls.get_nowait()()
        self.assertFalse(show.call_args.args[2])

    def test_profile_rename_is_atomic_and_never_overwrites_another(self):
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(launcher,'CONFIG_DIR',Path(folder)):
            ui=self.make_launcher()
            ui.save_controller_preset(create=True)
            ui.save_controller_preset(create=True)
            profiles=launcher.read_controller_presets()
            ui.controller_preset_name.set('Profile 1')
            for name in (' ', 'profile 2', 'x'*81, 'bad\nname', None):
                with mock.patch.object(ui,'controller_dialog',return_value=name):
                    ui.rename_controller_preset()
                self.assertEqual(launcher.read_controller_presets(),profiles)
            with mock.patch.object(ui,'controller_dialog',return_value='José — Xbox'), \
                 mock.patch.object(launcher.os,'replace',side_effect=OSError('read only')):
                ui.rename_controller_preset()
            self.assertEqual(launcher.read_controller_presets(),profiles)
            self.assertEqual(ui.controller_preset_name.get(),'Profile 1')
            with mock.patch.object(ui,'controller_dialog',return_value='José — Xbox'):
                ui.rename_controller_preset()
            saved=launcher.read_controller_presets()
            self.assertEqual(saved['José — Xbox'],profiles['Profile 1'])
            self.assertEqual(saved['Profile 2'],profiles['Profile 2'])
            self.assertNotIn('Profile 1',saved)

    def test_profile_dialog_style_keyboard_validation_and_cleanup(self):
        import re
        ui=self.make_launcher()
        def descendants(widget):
            return [child for w in widget.winfo_children() for child in [w,*descendants(w)]]
        def key(widget,sequence):
            # Invoke Tk's registered key handler without mapping a window onto the desktop.
            command=re.search(r'\[([^ ]+)',widget.bind(sequence))[1]
            args=[str(widget) if code=='%W' else '0' for code in widget._subst_format]
            widget.tk.call(command,*args)
        for scale in (1,1.25,1.5,2):
            ui.dpi=scale
            def rename(window):
                self.assertEqual(window['bg'],launcher.CARD)
                children=descendants(window)
                entry=next(w for w in children if isinstance(w,tk.Entry))
                self.assertEqual(entry['bg'],launcher.BG)
                self.assertTrue(entry.selection_present())
                entry.delete(0,'end')
                key(window,'<Return>')
                self.assertTrue(window.winfo_exists())
                self.assertTrue(any(isinstance(w,tk.Label) and '80' in w['text'] for w in children))
                entry.insert(0,'  José — Xbox  ')
                key(window,'<Return>')
            with mock.patch.object(self.root,'wait_window',side_effect=rename):
                self.assertEqual(ui.controller_dialog('Rename profile','New profile name:','Save name',initial='Profile 1'),
                                 'José — Xbox')
            def cancel(window):
                buttons=[w for w in descendants(window) if isinstance(w,tk.Canvas)]
                self.assertEqual(len(buttons),2)
                self.assertTrue(all(w['takefocus']=='1' for w in buttons))
                key(buttons[0],'<Return>')  # Cancel has its own handler, not the default action.
            with mock.patch.object(self.root,'wait_window',side_effect=cancel):
                self.assertIsNone(ui.controller_dialog('Delete profile','Keep current settings.','Delete profile'))
            with mock.patch.object(self.root,'wait_window',side_effect=lambda w:key(w,'<Escape>')):
                self.assertIsNone(ui.controller_dialog('Rename profile','Name','Save name',initial='Profile 1'))
            with mock.patch.object(self.root,'wait_window',side_effect=lambda w:key(w,'<Return>')):
                self.assertTrue(ui.controller_dialog('Save changes','Replace profile?','Save changes'))
            self.assertFalse(any(isinstance(w,tk.Toplevel) for w in self.root.winfo_children()))

    def test_unapplied_changes_survive_selection_and_require_discard(self):
        ui=self.make_launcher()
        ui.var('pad_mapping_mode').set('custom')
        ui.update_controller_ui()
        box=ui.binding_boxes[0]
        box.current(box._pad_values.index('y'))
        box.event_generate('<<ComboboxSelected>>')
        self.assertEqual(ui.mapping_drafts['cross'],('y','none'))
        self.assertEqual(ui.pad_vars['cross'].get(),'a')
        ui.controller_diagram.select('b')
        self.assertIn('Cross',ui.mapping_hint['text'])
        ui.mapping_action.current(0)
        ui.mapping_action.event_generate('<<ComboboxSelected>>')
        self.assertEqual(box._pad_values[box.current()],'y')
        ui.var('controller_model').set('ps5')
        ui.update_controller_ui()
        self.assertEqual(box._pad_values[box.current()],'y')
        destroy_binding=self.root.bind('<Destroy>')
        ui.app['ui_language']='en'
        # Rebuilding the page must keep drafts without adding root-level callbacks.
        with mock.patch.object(ui,'collect'),mock.patch.object(ui,'refresh'), \
             mock.patch.object(ui,'detect_controller') as detect:
            ui.change_ui_language()
        detect.assert_called_once_with()
        self.assertEqual(self.root.bind('<Destroy>'),destroy_binding)
        self.assertEqual(ui.mapping_drafts['cross'],('y','none'))
        ui.var('pad_mapping_mode').set('custom')
        ui.update_controller_ui()
        box=ui.binding_boxes[0]
        with mock.patch.object(ui,'controller_dialog',return_value=False):
            ui.reset_pad()
            self.assertFalse(ui.discard_mapping_drafts())
        self.assertEqual(ui.mapping_drafts['cross'],('y','none'))
        ui.apply_mapping()
        self.assertFalse(ui.mapping_drafts)
        self.assertEqual(ui.pad_vars['cross'].get(),'y')
        box.current(box._pad_values.index('x'))
        box.event_generate('<<ComboboxSelected>>')
        with mock.patch.object(ui,'controller_dialog',return_value=True):
            self.assertTrue(ui.discard_mapping_drafts())
        self.assertEqual(ui.pad_vars['cross'].get(),'y')
        self.assertEqual(box._pad_values[box.current()],'y')

    def test_pressed_highlights_are_separate_and_bounded(self):
        ui=self.make_launcher()
        diagram=ui.controller_diagram
        diagram.select('a')
        before=diagram.canvas.find_all()
        callbacks=tuple(diagram.canvas._tclCommands)
        for _i in range(1000):
            diagram.show_pressed({'b','lefttrigger','unknown'})
            self.assertEqual(len(diagram.canvas.find_withtag('pressed')),2)
        self.assertEqual(diagram.selected,'a')
        self.assertEqual(tuple(diagram.canvas._tclCommands),callbacks)
        diagram.show_pressed(())
        self.assertEqual(diagram.canvas.find_all(),before)

    def test_button_probe_results_errors_timeout_and_stale_callbacks(self):
        import io
        ui=self.make_launcher()
        def inline(target,**_kw):
            return SimpleNamespace(start=target)
        def drain():
            while not ui.ui_calls.empty():
                ui.ui_calls.get_nowait()()
        for output,code,message in (
            ('pressed=a,lefttrigger,\npressed=\n',0,'completed'),
            ('watch_error=Controller disconnected\n',0,'disconnected'),
            ('count=0\n',0,'No button data'),
            ('',1,'No button data')):
            process=mock.Mock(stdout=io.StringIO(output))
            process.wait.return_value=code
            process.poll.return_value=code
            with mock.patch.object(launcher,'controller_probe_path',return_value=Path('probe.exe')), \
                 mock.patch.object(launcher.threading,'Thread',side_effect=inline), \
                 mock.patch.object(launcher.threading,'Timer'), \
                 mock.patch.object(launcher.subprocess,'Popen',return_value=process), \
                 mock.patch.object(ui.controller_diagram,'show_pressed',wraps=ui.controller_diagram.show_pressed) as pressed:
                ui.test_controller_buttons()
                drain()
                self.assertIsNone(ui.controller_watch)
                self.assertIn(message,ui.controller_watch_status['text'])
                if output.startswith('pressed'):
                    self.assertIn(mock.call({'a','lefttrigger'}),pressed.call_args_list)
        process=mock.Mock(stdout=io.StringIO('pressed=a\n'))
        process.wait.return_value=0
        process.poll.return_value=None
        def timeout(_seconds,callback):
            return SimpleNamespace(start=callback,cancel=lambda:None)
        with mock.patch.object(launcher,'controller_probe_path',return_value=Path('probe.exe')), \
             mock.patch.object(launcher.threading,'Thread',side_effect=inline), \
             mock.patch.object(launcher.threading,'Timer',side_effect=timeout), \
             mock.patch.object(launcher.subprocess,'Popen',return_value=process):
            ui.test_controller_buttons()
            drain()
            self.assertIn('timed out',ui.controller_watch_status['text'])
            process.kill.assert_called()
        with mock.patch.object(launcher,'controller_probe_path',return_value=Path('probe.exe')), \
             mock.patch.object(launcher.threading,'Thread',side_effect=inline), \
             mock.patch.object(launcher.subprocess,'Popen',side_effect=OSError('cannot start')):
            ui.test_controller_buttons()
            ui.stop_controller_buttons()
            ui.controller_watch_status.configure(text='new session')
            drain()
            self.assertEqual(ui.controller_watch_status['text'],'new session')



if __name__ == '__main__':
    unittest.main()
