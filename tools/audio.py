#!/usr/bin/env python3
"""Extrae el audio de los bancos de FMOD (music.bank, sfx.bank) y lo deja listo para la 3DS.

Uso: tools/.venv/bin/python -I tools/audio.py <carpeta Content> <carpeta romfs>

  - vgmstream-cli saca cada muestra a WAV,
  - oggenc la vuelve a comprimir en Ogg Vorbis (la 3DS la decodifica con Tremor),
  - romfs:/audio/events.txt dice qué muestras suenan con cada evento del juego.

El proyecto de FMOD Studio no es público, así que la relación evento -> muestras se arma aquí
a partir de los nombres (Source/Audio/Events.cs y los nombres de las muestras del banco).
"""
import os
import re
import subprocess
import sys

VGM = 'vgmstream-cli'
OGGENC = 'oggenc'

# música: nombre del evento -> (muestra del banco, se repite)
MUSIC = {
    'mus_lvl1': 'Celeste_Fragments_of_the_Mountain_v2',
    'mus_lvl1_bside': 'Celeste_64_-_Cassette_Jam_v1',
    'mus_title': 'Celeste_Fragments_-_Title_Screen',
    'mus_lvl1_baddy': 'Celeste_64_-_Inner_Goth',
}

# efectos: constante de Sfx (Events.cs) -> (bucle, volumen, [muestras; se elige una al azar])
EVENTS = {
    'ui_pause': (0, 1.0, ['sfx_pause_v2']),
    'ui_unpause': (0, 1.0, ['sfx_unpause_v2']),
    'ui_move': (0, 1.0, ['ui_main_roll_up']),
    'ui_select': (0, 1.0, ['ui_pausemenu_select']),
    'ui_npc_popup': (0, 1.0, ['ui_npc_popup']),
    'ui_dialog_advance': (0, 1.0, ['ui_dialog_advance']),
    'ui_spotlight_in': (0, 1.0, ['game_gen_spotlight_intro_in']),
    'ui_spotlight_out': (0, 1.0, ['game_gen_spotlight_outro_in']),
    'main_menu_first_input': (0, 1.0, ['ui_main_gameboot']),
    'main_menu_postcard_flip': (0, 1.0, ['ui_main_postcard_flip']),
    'main_menu_postcard_flip_back': (0, 1.0, ['ui_main_postcard_flip_back']),
    'main_menu_start_game': (0, 1.0, ['ui_main_startgame']),
    'main_menu_toggle_on': (0, 1.0, ['ui_main_toggle_on']),
    'main_menu_toggle_off': (0, 1.0, ['ui_main_toggle_off']),
    'main_menu_roll_up': (0, 1.0, ['ui_main_roll_up']),
    'main_menu_roll_down': (0, 1.0, ['ui_main_roll_down']),
    'main_menu_restart_confirm_popup': (0, 1.0, ['ui_main_restart_confirm_popup']),
    'main_menu_restart_cancel': (0, 1.0, ['ui_main_restart_cancel']),
    'amb_mountain': (1, 0.8, ['env_amb_01_main']),
    'sfx_jump': (0, 1.0, ['char_mad_jump']),
    'sfx_jump_wall': (0, 1.0, ['char_mad_jump_wall_left']),
    'sfx_jump_assisted': (0, 1.0, ['char_mad_jump_assisted']),
    'sfx_jump_superslide': (0, 1.0, ['char_mad_jump_superslide']),
    'sfx_jump_skid': (0, 1.0, ['char_mad_skid_jump']),
    'sfx_land': (0, 1.0, ['char_mad_land_03_books_03']),
    'sfx_grab': (0, 1.0, ['char_mad_grab_03_books_02']),
    'sfx_skid': (0, 1.0, ['char_mad_skid', 'char_mad_skid_v2', 'char_mad_skid_v3']),
    'sfx_let_go': (0, 1.0, ['char_mad_grab_letgo_01']),
    'sfx_climb_ledge': (0, 1.0, ['char_mad_climb_ledge_02']),
    'sfx_wall_slide': (1, 1.0, ['char_mad_wallslide_general']),
    'sfx_handhold': (0, 1.0, ['char_mad_handhold_03_books_02']),
    'sfx_dash_red': (0, 1.0, ['char_mad_dash_red_left']),
    'sfx_dash_pink': (0, 1.0, ['char_mad_dash_pink_left']),
    'sfx_death': (0, 1.0, ['char_mad_death_v2']),
    'sfx_revive': (0, 1.0, ['char_mad_revive']),
    'sfx_bubble_in': (0, 1.0, ['sfx_bubble_in']),
    'sfx_bubble_loop': (1, 1.0, ['sfx_bubble_loop_alt']),
    'sfx_bubble_out': (0, 1.0, ['sfx_bubble_out']),
    'sfx_glassbreak': (0, 1.0, ['sfx_glassbreak']),
    'sfx_secret': (0, 1.0, ['game_gen_secret_revealed']),
    'sfx_feather_get': (0, 1.0, ['game_06_feather_get']),
    'sfx_feather_renew': (0, 1.0, ['game_06_feather_renew']),
    'sfx_feather_reappear': (0, 1.0, ['game_06_feather_reappear']),
    'sfx_feather_state_active_loop': (1, 1.0, ['game_06_feather_state_fast_loop']),
    'sfx_feather_state_end_warning': (0, 1.0, ['game_06_feather_state_warning']),
    'sfx_feather_state_end': (0, 1.0, ['game_06_feather_state_end']),
    'sfx_feather_state_bump_wall': (0, 1.0, ['game_06_feather_state_bump_0%d' % i for i in range(1, 6)]),
    'sfx_breakable_wall_dirt': (0, 1.0, ['game_gen_wallbreak_dirt']),
    'sfx_breakable_wall_wood': (0, 1.0, ['game_gen_wallbreak_wood']),
    'sfx_touch_switch_any': (0, 1.0, ['game_gen_touchswitch_any']),
    'sfx_touch_switch_last': (0, 1.0, ['game_gen_touchswitch_last_2d']),
    'sfx_touch_switch_gate_open_move': (0, 1.0, ['game_gen_touchswitch_gate_open']),
    'sfx_touch_switch_gate_finish': (0, 1.0, ['game_gen_touchswitch_gate_finish']),
    'sfx_cassette_enter': (0, 1.0, ['sfx_cassette_enter']),
    'sfx_cassette_exit': (0, 1.0, ['sfx_cassette_exit']),
    'sfx_dashcrystal': (0, 1.0, ['game_gen_diamond_touch_01']),
    'sfx_dashcrystal_return': (0, 1.0, ['game_gen_diamond_return_01']),
    'sfx_dashcrystal_double': (0, 1.0, ['game_10_pinkdiamond_touch_01']),
    'sfx_dashcrystal_double_return': (0, 1.0, ['game_10_pinkdiamond_return_01']),
    'sfx_fallingblock_shake': (0, 1.0, ['game_01_fallingblock_ice_shake_01']),
    'sfx_fallingblock_fall': (0, 1.0, ['game_00_fallingblock_prologue_release']),
    'sfx_fallingblock_land': (0, 1.0, ['game_01_fallingblock_ice_impact_01']),
    'sfx_springboard': (0, 1.0, ['game_gen_spring']),
    'sfx_zipmover_start': (0, 1.0, ['game_01_zipmover_a_touch_01']),
    'sfx_zipmover_stop': (0, 1.0, ['game_01_zipmover_b_impact_01']),
    'sfx_zipmover_retract_start': (0, 1.0, ['game_01_zipmover_c_return_0%d' % i for i in range(1, 4)]),
    'sfx_zipmover_retract_stop': (0, 1.0, ['game_01_zipmover_d_reset_01']),
    'sfx_readsign_in': (0, 1.0, ['ui_game_textbox_other_in']),
    'sfx_readsign_out': (0, 1.0, ['ui_game_textbox_other_out']),
    'sfx_collect_strawb': (0, 1.0, ['strawb_get_v2']),
    'sfx_collect_strawb_bside': (0, 1.0, ['Celeste_64_-_Cassette_Strawb_GET']),
    'sfx_berry_appear': (0, 1.0, ['strawb_alt_transient']),
    'sfx_checkpoint': (0, 1.0, ['flag_finish']),
    'sfx_car_up': (0, 1.0, ['game_00_car_up_0%d' % i for i in range(1, 6)]),
    'sfx_car_down': (0, 1.0, ['game_00_car_down_0%d' % i for i in range(1, 6)]),
}


def streams(bank):
    """[(índice, nombre)] de las muestras del banco."""
    out = subprocess.run([VGM, '-m', bank], capture_output=True, text=True).stdout
    count = int(re.search(r'stream count: (\d+)', out).group(1))
    result = []
    for i in range(1, count + 1):
        info = subprocess.run([VGM, '-m', '-s', str(i), bank], capture_output=True, text=True).stdout
        result.append((i, re.search(r'stream name: (.*)', info).group(1).strip()))
    return result


def encode(bank, index, out_ogg, work, rate, quality, downmix=False):
    wav = os.path.join(work, 'tmp.wav')
    subprocess.run([VGM, '-s', str(index), '-o', wav, bank], check=True, capture_output=True)
    cmd = [OGGENC, '-Q', '-q', str(quality), '--resample', str(rate), '-o', out_ogg, wav]
    if downmix:
        cmd.insert(1, '--downmix')
    subprocess.run(cmd, check=True, stderr=subprocess.DEVNULL)   # avisa al pasar a mono algo que ya es mono


def safe(name):
    return re.sub(r'[^A-Za-z0-9_.-]', '_', name)


def main():
    content, romfs = sys.argv[1], sys.argv[2]
    audio = os.path.join(content, 'Audio')
    work = os.path.join(os.path.dirname(os.path.abspath(romfs)), 'build', 'audio')
    os.makedirs(work, exist_ok=True)
    out_music = os.path.join(romfs, 'audio', 'music')
    out_sfx = os.path.join(romfs, 'audio', 'sfx')
    os.makedirs(out_music, exist_ok=True)
    os.makedirs(out_sfx, exist_ok=True)

    music_bank = os.path.join(audio, 'music.bank')
    by_name = {n: i for i, n in streams(music_bank)}
    for event, sample in MUSIC.items():
        encode(music_bank, by_name[sample], os.path.join(out_music, event + '.ogg'), work, 32000, 3)

    sfx_bank = os.path.join(audio, 'sfx.bank')
    sfx = streams(sfx_bank)
    by_name = {}
    for i, n in sfx:
        by_name.setdefault(n, i)
    needed = {s for _, _, samples in EVENTS.values() for s in samples}
    # voces de los diálogos (Language.Line.Voice -> event:/sfx/ui/dialog/<voz>): sus muestras se llaman igual
    voices = [n for n in by_name if re.match(r'(granny|theo|baddy)\d_\d+_|vo_sign_', n)]
    needed |= set(voices)
    missing = sorted(s for s in needed if s not in by_name)
    for name in sorted(needed - set(missing)):
        # el ambiente largo va en estéreo; lo demás a mono para ahorrar memoria y CPU
        stereo = name in ('env_amb_01_main',)
        encode(sfx_bank, by_name[name], os.path.join(out_sfx, safe(name) + '.ogg'), work, 22050, 2, downmix=not stereo)

    with open(os.path.join(romfs, 'audio', 'events.txt'), 'w') as f:
        for event, (loop, volume, samples) in EVENTS.items():
            ok = [safe(s) for s in samples if s in by_name]
            if ok:
                f.write(f'{event}|{loop}|{volume}|{",".join(ok)}\n')

    size = sum(os.path.getsize(os.path.join(d, x)) for d, _, fs in os.walk(os.path.join(romfs, 'audio')) for x in fs)
    print(f'audio: {len(MUSIC)} pistas, {len(needed) - len(missing)} efectos/voces, {size / 1024 / 1024:.1f} MB')
    if missing:
        print('  sin muestra:', ', '.join(missing))


if __name__ == '__main__':
    main()
