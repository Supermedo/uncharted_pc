"""Small launcher translation table with English fallback."""
import locale


LANGUAGES = [
    ('', 'Automatic (Windows)'),
    ('en', 'English'),
    ('es', 'Español'),
]


def language_code(value=''):
    if value in {code for code, _name in LANGUAGES if code}:
        return value
    current = locale.getlocale()[0] or 'en'
    code = current.split('_', 1)[0].lower()
    return code if code == 'es' else 'en'


TRANSLATIONS = {
    'es': {
        'Automatic (Windows)': 'Automático (Windows)', 'Play': 'Jugar', 'Stop': 'Detener',
        'Game': 'Juego', 'Display': 'Pantalla', 'Controls': 'Controles', 'Advanced': 'Avanzado',
        'Log': 'Registro', 'Library': 'Biblioteca', 'Double-click a cover to launch':
        'Haz doble clic en una portada para iniciar', 'Game folder': 'Carpeta del juego',
        'The folder with eboot.bin': 'La carpeta que contiene eboot.bin',
        'Saves folder': 'Carpeta de partidas', 'Console': 'Consola', 'Language': 'Idioma',
        'Launcher language': 'Idioma del launcher', 'Interface language': 'Idioma de la interfaz',
        'This changes only the launcher. The game language is selected on the Game page.':
        'Esto cambia solo el launcher. El idioma del juego se elige en la página Juego.',
        'Player name': 'Nombre del jugador',
        'Fullscreen': 'Pantalla completa', 'Presentation': 'Presentación',
        'Frame rate': 'Frecuencia de cuadros', 'Resolution': 'Resolución',
        'Window size': 'Tamaño de ventana', 'Upscaling': 'Reescalado',
        'Sharpening': 'Nitidez', 'Anti-aliasing': 'Suavizado de bordes',
        'Anti-aliasing sharpness': 'Nitidez del suavizado',
        'Frame generation': 'Generación de cuadros', 'Multiplier': 'Multiplicador',
        'Show the FPS counter': 'Mostrar contador de FPS', 'Mouse': 'Ratón',
        'Look around with the mouse': 'Mover la cámara con el ratón',
        'Mouse sensitivity': 'Sensibilidad del ratón',
        'Invert the mouse vertically': 'Invertir verticalmente el ratón',
        'Keyboard and mouse buttons': 'Teclado y botones del ratón',
        'Controller': 'Control', 'Default keys': 'Teclas predeterminadas',
        'Default controller buttons': 'Botones predeterminados del control',
        'Maintenance and options for testing.': 'Mantenimiento y opciones para pruebas.',
        'Close the launcher when the game starts': 'Cerrar el launcher al iniciar el juego',
        'Environment': 'Entorno', 'Shader cache': 'Caché de shaders', 'Updates': 'Actualizaciones',
        'Clear shader cache': 'Borrar caché de shaders',
        'Open the saves folder': 'Abrir carpeta de partidas',
        'Look for a new version when the launcher starts': 'Buscar actualizaciones al abrir el launcher',
        'Check for updates': 'Buscar actualizaciones', 'Releases page': 'Página de versiones',
        'Browse…': 'Examinar…', 'Open': 'Abrir', 'Add to Steam': 'Añadir a Steam',
        'Desktop shortcut': 'Acceso directo en el escritorio', 'Copy': 'Copiar',
        'Choose the game folder (the one with eboot.bin) on the Game page.':
        'Elige la carpeta del juego (la que contiene eboot.bin) en la página Juego.',
        'No game folder yet.': 'Aún no se ha elegido la carpeta del juego.',
        'The game is running.': 'El juego está en ejecución.',
        'Native port: the original PS4 game code runs directly on Windows.':
        'Port nativo: el código original de PS4 se ejecuta directamente en Windows.',
        'version': 'versión', 'version {}': 'versión {}',
        '\n— the game exited (code {}) —\n': '\n— el juego terminó (código {}) —\n',
        'AMD RCAS after FSR 1 (only while the image is enlarged).':
        'AMD RCAS después de FSR 1 (solo cuando se amplía la imagen).',
        'Choose the game folder with eboot.bin.': 'Elige la carpeta del juego que contiene eboot.bin.',
        'Click a box, then press a key or a mouse button (the wheel counts too). ✕ removes a binding. Each action can have two.':
        'Haz clic en una casilla y pulsa una tecla o botón del ratón (la rueda también cuenta). ✕ elimina una asignación. Cada acción puede tener dos.',
        'Close the game before updating.': 'Cierra el juego antes de actualizar.',
        'Compiled shaders are kept so the game does not stutter again. Clear the cache only when the picture is broken after an update.':
        'Los shaders compilados se conservan para evitar nuevos tirones. Borra la caché solo si la imagen falla después de una actualización.',
        'Could not add the game to Steam: {}': 'No se pudo añadir el juego a Steam: {}',
        'Could not check for updates: {}': 'No se pudo buscar actualizaciones: {}',
        'Could not install the update. Download it from the releases page.':
        'No se pudo instalar la actualización. Descárgala desde la página de versiones.',
        'Could not start: {}': 'No se pudo iniciar: {}',
        'Downloading version {}… {}%': 'Descargando la versión {}… {}%',
        'Empty: the user folder next to the launcher.': 'Vacío: se usará la carpeta user junto al launcher.',
        'Extra variables for the game, NAME=value separated by spaces.':
        'Variables adicionales para el juego, NOMBRE=valor separadas por espacios.',
        'FSR 1 keeps edges sharp when the image is enlarged; bilinear is softer.':
        'FSR 1 mantiene los bordes nítidos al ampliar la imagen; bilineal es más suave.',
        'FSR 3 makes one frame (2×). DLSS makes up to three on a GeForce RTX 50 (4×), one on an RTX 40.':
        'FSR 3 genera un cuadro (2×). DLSS genera hasta tres en una GeForce RTX 50 (4×) y uno en una RTX 40.',
        "Frames made between two of the game's, for smoother motion: 60 FPS shows as 120. Use Mailbox or Immediate presentation and a display of at least that refresh rate. Adds a little input latency; menus and cutscene videos are not generated. Use it with the 60 or 120 FPS frame rate: above about 165 FPS it pauses.":
        'Cuadros creados entre dos del juego para un movimiento más fluido: 60 FPS se muestran como 120. Usa presentación Mailbox o Inmediata y una pantalla con al menos esa frecuencia. Añade un poco de latencia; no se generan cuadros en menús ni cinemáticas. Úsalo con 60 o 120 FPS: por encima de unos 165 FPS se pausa.',
        "Gameplay keeps its normal speed at every setting; the cutscene videos always play at 60 FPS. Above your display's refresh rate use Mailbox or Immediate.":
        'El juego mantiene su velocidad normal con cualquier ajuste; las cinemáticas siempre se reproducen a 60 FPS. Por encima de la frecuencia de tu pantalla usa Mailbox o Inmediata.',
        'How far the camera turns for a mouse movement; 1.00 is the default.':
        'Cuánto gira la cámara con el movimiento del ratón; 1.00 es el valor predeterminado.',
        'How the game window is shown.': 'Cómo se muestra la ventana del juego.',
        'Install version {} now? The launcher closes, installs it and opens again. Saves and settings are kept.':
        '¿Instalar ahora la versión {}? El launcher se cerrará, la instalará y volverá a abrirse. Se conservarán las partidas y los ajustes.',
        'Invert the right stick vertically': 'Invertir verticalmente el stick derecho',
        'Keyboard, mouse and controller. They work together; changes apply when the game starts.':
        'Teclado, ratón y control. Funcionan a la vez; los cambios se aplican al iniciar el juego.',
        'Mailbox shows each frame as soon as it is ready without tearing; FIFO waits for the display.':
        'Mailbox muestra cada cuadro en cuanto está listo sin tearing; FIFO espera a la pantalla.',
        'Not in this folder: ': 'No está en esta carpeta: ',
        'Preparing the game; it opens in its own window…': 'Preparando el juego; se abrirá en su propia ventana…',
        'Press a key…': 'Pulsa una tecla…', 'Ready: {}': 'Listo: {}',
        'Set your game folder on the Game page': 'Elige la carpeta del juego en la página Juego',
        'Shader cache cleared. The next start stutters for a while as it is rebuilt.':
        'Caché de shaders borrada. El próximo inicio tendrá tirones mientras vuelve a generarse.',
        'Shortcut created on the desktop.': 'Acceso directo creado en el escritorio.',
        'Shown where the game uses the PSN name.': 'Se muestra donde el juego utiliza el nombre de PSN.',
        'Starting the next game of the collection…': 'Iniciando el siguiente juego de la colección…',
        'Steam did not close. Close it yourself and try again.': 'Steam no se cerró. Ciérralo e inténtalo de nuevo.',
        'Steam has to close while the game is added; it starts again afterwards. Close Steam now?':
        'Steam debe cerrarse mientras se añade el juego; después volverá a abrirse. ¿Cerrar Steam ahora?',
        'Steam was not found on this PC (or nobody has signed in to it yet).':
        'No se encontró Steam en este PC (o nadie ha iniciado sesión todavía).',
        "Temporal anti-aliasing at the games' 1920 × 1080, with their own motion vectors: smoother edges and less shimmering than the games' own. DLSS needs a GeForce RTX; FSR 4 runs best on a Radeon RX 7000/9000 or a GeForce RTX 40/50.":
        'Suavizado temporal a 1920 × 1080 con los vectores de movimiento del juego: bordes más suaves y menos parpadeo que el suavizado original. DLSS requiere una GeForce RTX; FSR 4 funciona mejor en Radeon RX 7000/9000 o GeForce RTX 40/50.',
        'The collection is in your Steam library. It starts with the settings saved in this launcher.':
        'La colección está en tu biblioteca de Steam. Se iniciará con los ajustes guardados en este launcher.',
        'The cursor stays in the game window; hold Left Alt (or switch windows) to free it. Insert opens the in-game settings menu.':
        'El cursor permanece en la ventana del juego; mantén Alt izquierdo (o cambia de ventana) para liberarlo. Insert abre los ajustes dentro del juego.',
        'The folder with eboot.bin, big2-ps4_Shipping.elf and big3-ps4_Shipping.elf.':
        'La carpeta con eboot.bin, big2-ps4_Shipping.elf y big3-ps4_Shipping.elf.',
        'The game stopped: {}': 'El juego se detuvo: {}',
        'The games render at 1920 × 1080. A larger window, or fullscreen on a 1440p or 4K screen, gets the image upscaled to fit.':
        'Los juegos renderizan a 1920 × 1080. En una ventana mayor o a pantalla completa en 1440p o 4K, la imagen se reescala para ajustarse.',
        'The language the PS4 would be set to; the games use it for text and speech.':
        'El idioma que tendría configurado la PS4; los juegos lo usan para el texto y las voces.',
        'This game is not in the chosen folder.': 'Este juego no está en la carpeta elegida.',
        'Update': 'Actualizar', 'Update failed: {}': 'La actualización falló: {}',
        'Version {} is available.': 'La versión {} está disponible.', "What's new": 'Novedades',
        'Where saves and the shader cache are kept': 'Dónde se guardan las partidas y la caché de shaders',
        'Which controller button gives each PS4 button (Xbox / PlayStation names).':
        'Qué botón del control corresponde a cada botón de PS4 (nombres de Xbox / PlayStation).',
        'You have the latest version ({}).': 'Tienes la versión más reciente ({}).',
        'Your display runs at {} Hz.': 'Tu pantalla funciona a {} Hz.',
        'Your own decrypted dump of the collection (CUSA02343, CUSA02320, …).':
        'Tu propia copia descifrada de la colección (CUSA02343, CUSA02320, …).',
        'by {}': 'por {}', 'see the Log page': 'consulta la página Registro',
    },
}


def translate(language, text):
    return TRANSLATIONS.get(language, {}).get(text, text)
